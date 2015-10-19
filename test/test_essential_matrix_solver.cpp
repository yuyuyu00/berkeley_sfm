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
 * Authors: David Fridovich-Keil   ( dfk@eecs.berkeley.edu )
 *          Erik Nelson            ( eanelson@eecs.berkeley.edu )
 */

#include <camera/camera.h>
#include <camera/camera_extrinsics.h>
#include <camera/camera_intrinsics.h>
#include <geometry/fundamental_matrix_solver.h>
#include <geometry/essential_matrix_solver.h>
#include <geometry/eight_point_algorithm_solver.h>
#include <math/random_generator.h>
#include <matching/pairwise_image_match.h>

#include <Eigen/Core>
#include <gtest/gtest.h>

namespace bsfm {

namespace {
const int kImageWidth = 1920;
const int kImageHeight = 1080;
const double kVerticalFov = 90.0 * M_PI / 180.0;
const int kFeatureMatches = 200;
} //\namespace

TEST(EssentialMatrixSolver, TestEssentialMatrixSolver) {
  // Start out by using the eight point algorithm to estimate the fundamental matrix.

  // Create a random number generator.
  math::RandomGenerator rng(0);

  // Create two cameras. By default they will have identity extrinsics.
  Camera camera1;
  Camera camera2;

  // Give the two cameras the same intrinsics.
  CameraIntrinsics intrinsics;
  intrinsics.SetImageLeft(0);
  intrinsics.SetImageTop(0);
  intrinsics.SetImageWidth(kImageWidth);
  intrinsics.SetImageHeight(kImageHeight);
  intrinsics.SetVerticalFOV(kVerticalFov);
  intrinsics.SetFU(intrinsics.f_v());
  intrinsics.SetCU(0.5 * kImageWidth);
  intrinsics.SetCV(0.5 * kImageHeight);

  camera1.SetIntrinsics(intrinsics);
  camera2.SetIntrinsics(intrinsics);

  // Translate the 2nd camera along its X axis (give it some known base-line).
  // Camera 2 will be 200.0 pixels to the right of camera 1.
  camera2.MutableExtrinsics().TranslateX(200.0);

  // Create a bunch of points in 3D.
  PairwiseImageMatch match_data;
  while (match_data.feature_matches_.size() < kFeatureMatches) {
    // Since the camera's +Z faces down the world's -Y
    // direction, make the points back there somewhere.

    const double x_world = rng.DoubleUniform(-2000.0, 2200.0);
    const double y_world = rng.DoubleUniform(-3000.0, -2000.0);
    const double z_world = rng.DoubleUniform(-2000.0, 2000.0);

    // Project each of the 3D points into the two cameras.
    double u1 = 0.0, v1 = 0.0;
    double u2 = 0.0, v2 = 0.0;
    const bool in_camera1 =
        camera1.WorldToImage(x_world, y_world, z_world, &u1, &v1);
    const bool in_camera2 =
        camera2.WorldToImage(x_world, y_world, z_world, &u2, &v2);

    // Make sure the point is visible to both cameras... we want feature
    // matches.
    if (!(in_camera1 && in_camera2)) {
      continue;
    }

    // Store this as a feature match.
    FeatureMatch match;
    match.feature1_.u_ = u1;
    match.feature1_.v_ = v1;
    match.feature2_.u_ = u2;
    match.feature2_.v_ = v2;
    match_data.feature_matches_.push_back(match);
  }

  // Use the solver to find the fundamental matrix for the two sets of points.
  EightPointAlgorithmSolver eight_point_solver;
  eight_point_solver.AddMatchedImagePair(match_data);

  // Test both with and without normalization.
  FundamentalMatrixSolverOptions options;
  eight_point_solver.SetOptions(options);

  Eigen::Matrix3d F;
  EXPECT_TRUE(eight_point_solver.ComputeFundamentalMatrix(match_data.feature_matches_, F));

    
  // For every feature pair, we should have x1^{T}*F*x2 = 0.
  for (const auto& match : match_data.feature_matches_) {
    Eigen::Vector3d x1, x2;
    x1 << match.feature1_.u_, match.feature1_.v_, 1;
    x2 << match.feature2_.u_, match.feature2_.v_, 1;
    
    EXPECT_NEAR(0.0, x2.transpose() * F * x1, 1e-8);
    EXPECT_NEAR(0.0, x2.transpose() * F * x1, 1e-8);
  }

  // Now, given the fundamental matrix, calculate the essential matrix.
  EssentialMatrixSolver essential_solver;
  Eigen::Matrix3d E = essential_solver.ComputeEssentialMatrix(F,
							      camera1.Intrinsics(),
							      camera2.Intrinsics());
  EXPECT_TRUE(F.isApprox(camera2.Intrinsics().IntrinsicsMatrix().transpose().inverse() *
			 E * camera1.Intrinsics().IntrinsicsMatrix().inverse()));

  // Check that we can calculate extrinsics from the essential matrix.
  CameraExtrinsics estimated_extrinsics;
  EXPECT_TRUE(essential_solver.ComputeExtrinsics(&estimated_extrinsics,
						 E, match_data.feature_matches_,
						 camera1.Intrinsics(), camera2.Intrinsics()));
  
  // Check that the estimated extrinsics are the same as the true extrinsics.
  EXPECT_TRUE(estimated_extrinsics.ExtrinsicsMatrix().isApprox(camera2.Extrinsics().ExtrinsicsMatrix()));
}

}  //\namespace bsfm
