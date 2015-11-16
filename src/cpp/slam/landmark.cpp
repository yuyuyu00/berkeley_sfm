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

#include "landmark.h"

#include <numeric>

#include "../sfm/view.h"

namespace bsfm {

// Declaration of static member variable.
std::unordered_map<LandmarkIndex, Landmark::Ptr> Landmark::landmark_registry_;
LandmarkIndex Landmark::current_landmark_index_ = 0;

// Factory method. Registers the landmark and newly created index in the
// landmark registry so that they can be accessed from the static GetLandmark()
// method. This guarantees that all landmarks will have unique indices.
Landmark::Ptr Landmark::Create() {
  // Create a new Landmark, implicitly assigning a unique index.
  Landmark::Ptr landmark(new Landmark());
  landmark_registry_.insert(std::make_pair(landmark->Index(), landmark));
  return landmark;
}

// Gets the landmark corresponding to the input index. If the landmark has not
// been created yet, this method returns a null pointer.
Landmark::Ptr Landmark::GetLandmark(LandmarkIndex landmark_index) {
  auto registry_element = landmark_registry_.find(landmark_index);
  if (registry_element == landmark_registry_.end()) {
    LOG(WARNING)
        << "Landmark does not exist in registry. Returning a null pointer.";
    return Landmark::Ptr();
  }

  return registry_element->second;
}

// Returns the total number of existing landmarks.
LandmarkIndex Landmark::NumExistingLandmarks() {
  return landmark_registry_.size();
}

// Returns a vector of all existing landmark indices.
std::vector<LandmarkIndex> Landmark::ExistingLandmarkIndices() {
  std::vector<LandmarkIndex> indices(NumExistingLandmarks());
  std::iota(indices.begin(), indices.end(), 0);
  return indices;
}

// Returns whether the landmark index corresponds to a landmark that has been
// created.
bool Landmark::IsValidLandmark(LandmarkIndex landmark_index) {
  return landmark_index <= current_landmark_index_;
}

// Resets all landmarks and clears the landmark registry. This should rarely be
// called, except when completely resetting the program or reconstruction.
void Landmark::ResetLandmarks() {
  current_landmark_index_ = 0;
  landmark_registry_.clear();
}

// Returns the unique index of this landmark.
LandmarkIndex Landmark::Index() const {
  return landmark_index_;
}

// Set the landmark's position.
void Landmark::SetPosition(const Point3D& position) {
  position_ = position;
}

// Set the landmark's descriptor.
void Landmark::SetDescriptor(const ::bsfm::Descriptor& descriptor) {
  descriptor_ = descriptor;
}

// Remove all existing observations of the landmark.
void Landmark::ClearObservations() {
  observations_.clear();
}

// Get position.
const Point3D& Landmark::Position() const {
  return position_;
}

// Get descriptor.
const ::bsfm::Descriptor& Landmark::Descriptor() const {
  return descriptor_;
}

// Get observations.
const std::vector<Observation::Ptr>& Landmark::Observations() const {
  return observations_;
}

// Returns a raw pointer to the data elements of the position of the landamark.
// This is useful for optimization on landmark positions (e.g. during bundle
// adjustment).
double* Landmark::PositionData() {
  return position_.Get().data();
}

// Add a new observation of the landmark. If 'retriangulate' is true, the
// landmark's position will be retriangulated from all observations of it.
bool Landmark::IncorporateObservation(const Observation::Ptr& observation,
                                      bool retriangulate) {
  CHECK_NOTNULL(observation.get());

  // If this is our first observation, store it, tell the observation that it
  // has been matched with us, and return.
  if (observations_.empty()) {
    observation->SetIncorporatedLandmark(this->Index());
    observations_.push_back(observation);
    descriptor_ = observation->Descriptor();
    return true;
  }

  // Does our own descriptor match with the observation's descriptor?
  DistanceMetric& distance = DistanceMetric::Instance();
  if (distance.Max() != std::numeric_limits<double>::max()) {
    std::vector<::bsfm::Descriptor> descriptors;
    descriptors.push_back(descriptor_);
    descriptors.push_back(observation->Descriptor());
    distance.MaybeNormalizeDescriptors(descriptors);

    if (distance(descriptors[0], descriptors[1]) > distance.Max()) {
      VLOG(1) << "Observation was not matched to landmark " << this->Index();
      return false;
    }
  }

  // Triangulate the landmark's putative position if we were to incorporate the
  // new observation.
  if (retriangulate) {
    std::vector<Camera> cameras;
    std::vector<Feature> features;
    for (const auto& obs : observations_) {
      cameras.push_back(obs->GetView()->Camera());
      features.push_back(obs->Feature());
    }
    cameras.push_back(observation->GetView()->Camera());
    features.push_back(observation->Feature());

    // If triangulation fails, we don't have a match and won't update position.
    Point3D new_position;
    if (!Triangulate(features, cameras, new_position)) {
      return false;
    }
    position_ = new_position;
  }

  // Successfully triangulated the landmark. Update its position and store this
  // observation. Also tell the observation that it has been matched with us.
  observation->SetIncorporatedLandmark(this->Index());
  observations_.push_back(observation);

  return true;
}

// Return the first view to observe this landmark.
View::Ptr Landmark::SourceView() const {
  if (observations_.empty()) {
    LOG(WARNING) << "Landmark has not been associated with any observations, "
                    "so source view is undefined. Returning null pointer.";
    return View::Ptr();
  }

  return observations_.front()->GetView();
}

// Given a set of views, return whether or not this landmark has been seen by at
// least N of them.
bool Landmark::SeenByAtLeastNViews(const std::vector<ViewIndex>& view_indices,
                                   unsigned int N) {
  unsigned int count = 0;
  for (const auto& view_index : view_indices) {
    // If they gave us a bad input, ignore it and continue.
    if (!View::IsValidView(view_index))
      continue;

    // Make sure this view knows all landmarks its observations have seen.
    View::Ptr view = View::GetView(view_index);
    view->UpdateObservedLandmarks();
    if (view->HasObservedLandmark(this->Index())) {
      count++;
    }

    if (count == N) {
      return true;
    }
  }

  return false;
}

// Private constructor enforces creation via factory method. This will be called
// from the factory method.
Landmark::Landmark()
    : position_(Point3D(0.0, 0.0, 0.0)), landmark_index_(NextLandmarkIndex()) {}

// Static method for determining the next index across all Landmarks constructed
// so far. This is called in the Landmark constructor.
LandmarkIndex Landmark::NextLandmarkIndex() {
  return current_landmark_index_++;
}

}  //\namespace bsfm
