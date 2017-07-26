//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) 2017 Guillaume Blanc                                         //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#include <cstdlib>
#include <future>

#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"

#include "ozz/base/log.h"

#include "ozz/base/containers/vector.h"

#include "ozz/base/maths/box.h"
#include "ozz/base/maths/math_ex.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/vec_float.h"

#include "ozz/base/memory/allocator.h"

#include "ozz/options/options.h"

#include "framework/application.h"
#include "framework/imgui.h"
#include "framework/renderer.h"
#include "framework/utils.h"

// Skeleton archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(skeleton,
                           "Path to the skeleton (ozz archive format).",
                           "media/skeleton.ozz", false)

// First animation archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(animation,
                           "Path to the first animation (ozz archive format).",
                           "media/animation.ozz", false)

// Interval between each character.
const float kInterval = 2.f;

// Width and depth of characters repartition.
const int kWidth = 16;
const int kDepth = 16;

// The maximum number of characters.
const int kMaxCharacters = 4096;

// Defines a TLS variable, which address is per thread and can thus be used to
// identify a thread.
thread_local size_t kTlsThreadIdentifier = 0;

class MultithreadSampleApplication : public ozz::sample::Application {
 public:
  MultithreadSampleApplication()
      : characters_(kMaxCharacters),
        thread_ids_(kMaxCharacters),
        num_characters_(1024),
        enable_theading_(true),
        grain_size_(128) {}

 private:
  // Nested Character struct forward declaration.
  struct Character;

  static bool UpdateCharacter(const ozz::animation::Animation* _animation,
                              const ozz::animation::Skeleton* _skeleton,
                              float _dt, Character* _character) {
    // Samples animation.
    _character->controller.Update(*_animation, _dt);

    // Setup sampling job.
    ozz::animation::SamplingJob sampling_job;
    sampling_job.animation = _animation;
    sampling_job.cache = _character->cache;
    sampling_job.time = _character->controller.time();
    sampling_job.output = _character->locals;

    // Samples animation.
    if (!sampling_job.Run()) {
      return false;
    }

    // Converts from local space to model space matrices.
    ozz::animation::LocalToModelJob ltm_job;
    ltm_job.skeleton = _skeleton;
    ltm_job.input = _character->locals;
    ltm_job.output = _character->models;
    if (!ltm_job.Run()) {
      return false;
    }

    return true;
  }

  static bool ParallelUpdate(int _grain_size,
                             const ozz::animation::Animation* _animation,
                             const ozz::animation::Skeleton* _skeleton,
                             float _dt, Character* _characters, int _num,
                             size_t** _thread_ids) {
    bool success = true;
    if (_num <= _grain_size) {
      size_t* thread_id = &kTlsThreadIdentifier;
      for (int i = 0; i < _num; ++i) {
        _thread_ids[i] = thread_id;  // Store this thread identifier.
        success &= UpdateCharacter(_animation, _skeleton, _dt, &_characters[i]);
      }
    } else {
      const int half = _num / 2;
      auto handle = std::async(std::launch::async, ParallelUpdate, _grain_size,
                               _animation, _skeleton, _dt, _characters + half,
                               _num - half, _thread_ids + half);
      success &= ParallelUpdate(_grain_size, _animation, _skeleton, _dt,
                                _characters, half, _thread_ids);
      success &= handle.get();
    }
    return success;
  }

 protected:
  // Updates current animation time.
  virtual bool OnUpdate(float _dt) {
    bool success = true;
    if (enable_theading_) {
      success = ParallelUpdate(grain_size_, &animation_, &skeleton_, _dt,
                               array_begin(characters_), num_characters_,
                               array_begin(thread_ids_));
    } else {
      for (int i = 0; i < num_characters_; ++i) {
        success &= UpdateCharacter(&animation_, &skeleton_, _dt,
                                   array_begin(characters_) + i);
      }
    }
    return success;
  }

  // Renders all skeletons.
  virtual bool OnDisplay(ozz::sample::Renderer* _renderer) {
    bool success = true;
    for (int c = 0; success && c < num_characters_; ++c) {
      ozz::math::Float4 position(
          ((c % kWidth) - kWidth / 2) * kInterval,
          ((c / kWidth) / kDepth) * kInterval,
          (((c / kWidth) % kDepth) - kDepth / 2) * kInterval, 1.f);
      ;
      const ozz::math::Float4x4 transform = ozz::math::Float4x4::Translation(
          ozz::math::simd_float4::LoadPtrU(&position.x));
      success &= _renderer->DrawPosture(skeleton_, characters_[c].models,
                                        transform, false);
    }

    return true;
  }

  virtual bool OnInitialize() {
    // Reading skeleton.
    if (!ozz::sample::LoadSkeleton(OPTIONS_skeleton, &skeleton_)) {
      return false;
    }

    // Reading animations.
    if (!ozz::sample::LoadAnimation(OPTIONS_animation, &animation_)) {
      return false;
    }

    // Allocate a default number of characters.
    AllocateCharaters();

    return true;
  }

  virtual void OnDestroy() { DeallocateCharaters(); }

  virtual bool OnGui(ozz::sample::ImGui* _im_gui) {
    // Exposes multi-threading parameters.
    {
      static bool oc_open = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "Threading control", &oc_open);
      if (oc_open) {
        _im_gui->DoCheckBox("Enables threading", &enable_theading_);
        if (enable_theading_) {
          char label[64];
          std::sprintf(label, "Grain size: %d", grain_size_);
          _im_gui->DoSlider(label, 64, kMaxCharacters, &grain_size_, .2f);

          // Finds number of threads, which is the number of unique thread ids
          // found.
          std::sort(thread_ids_.begin(), thread_ids_.begin() + num_characters_);
          auto end = std::unique(thread_ids_.begin(),
                                 thread_ids_.begin() + num_characters_);
          const int num_threads = static_cast<int>(end - thread_ids_.begin());
          int num_tasks = 1;
          for (int processed = grain_size_; processed < num_characters_;
               processed *= 2, num_tasks *= 2)
            ;
          std::sprintf(label, "Thread/task count: %d/%d", num_threads,
                       num_tasks);
          _im_gui->DoLabel(label);
        }
      }
    }

    // Exposes characters.
    {
      static bool oc_open = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "Sample control", &oc_open);
      if (oc_open) {
        char label[64];
        std::sprintf(label, "Number of entities: %d", num_characters_);
        _im_gui->DoSlider(label, 1, kMaxCharacters, &num_characters_, .7f);
        const int num_joints = num_characters_ * skeleton_.num_joints();
        std::sprintf(label, "Number of joints: %d", num_joints);
        _im_gui->DoLabel(label);
      }
    }
    return true;
  }

  virtual void GetSceneBounds(ozz::math::Box* _bound) const {
    _bound->min.x = -(kWidth / 2) * kInterval;
    _bound->max.x =
        _bound->min.x + ozz::math::Min(num_characters_, kWidth) * kInterval;
    _bound->min.y = 0.f;
    _bound->max.y = ((num_characters_ / kWidth / kDepth) + 1) * kInterval;
    _bound->min.z = -(kDepth / 2) * kInterval;
    _bound->max.z =
        _bound->min.z +
        ozz::math::Min(num_characters_ / kWidth, kDepth) * kInterval;
  }

 private:
  bool AllocateCharaters() {
    // Reallocate all characters.
    ozz::memory::Allocator* allocator = ozz::memory::default_allocator();
    for (size_t c = 0; c < kMaxCharacters; ++c) {
      Character& character = characters_[c];
      character.cache = allocator->New<ozz::animation::SamplingCache>(
          animation_.num_tracks());

      // Initializes each controller start time to a different value.
      character.controller.set_time(animation_.duration() * kWidth * c /
                                    kMaxCharacters);

      character.locals = allocator->AllocateRange<ozz::math::SoaTransform>(
          skeleton_.num_soa_joints());
      character.models =
          allocator->AllocateRange<ozz::math::Float4x4>(skeleton_.num_joints());
    }

    return true;
  }

  void DeallocateCharaters() {
    ozz::memory::Allocator* allocator = ozz::memory::default_allocator();
    for (size_t c = 0; c < kMaxCharacters; ++c) {
      Character& character = characters_[c];
      allocator->Delete(character.cache);
      allocator->Deallocate(character.locals);
      allocator->Deallocate(character.models);
    }
  }

  // Runtime skeleton.
  ozz::animation::Skeleton skeleton_;

  // Runtime animation.
  ozz::animation::Animation animation_;

  // Character structure contains all the data required to sample and blend a
  // character.
  struct Character {
    Character() : cache(NULL) {}

    // Playback animation controller. This is a utility class that helps with
    // controlling animation playback time.
    ozz::sample::PlaybackController controller;

    // Sampling cache.
    ozz::animation::SamplingCache* cache;

    // Buffer of local transforms which stores the blending result.
    ozz::Range<ozz::math::SoaTransform> locals;

    // Buffer of model space matrices. These are computed by the local-to-model
    // job after the blending stage.
    ozz::Range<ozz::math::Float4x4> models;
  };

  // Array of characters of the sample.
  ozz::Vector<Character>::Std characters_;

  // Number of used characters.
  int num_characters_;

  // Enable or disable threading.
  bool enable_theading_;

  // Define the number of characters that a task can handle.
  int grain_size_;

  // Array of thread identifiers used when updating characters.
  ozz::Vector<size_t*>::Std thread_ids_;
};

int main(int _argc, const char** _argv) {
  const char* title = "Ozz-animation sample: Multi-threading";
  return MultithreadSampleApplication().Run(_argc, _argv, "1.0", title);
}
