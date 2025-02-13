// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/vr/windows_mixed_reality/mixed_reality_input_helper.h"

#include <SpatialInteractionManagerInterop.h>
#include <windows.perception.h>
#include <windows.perception.spatial.h>
#include <windows.ui.input.spatial.h>

#include <wrl.h>
#include <wrl/event.h>

#include <unordered_map>
#include <vector>

#include "base/win/core_winrt_util.h"
#include "base/win/scoped_hstring.h"
#include "base/win/typed_event_handler.h"
#include "ui/gfx/transform.h"
#include "ui/gfx/transform_util.h"

namespace device {

// We want to differentiate from gfx::Members, so we're not going to explicitly
// use anything from Windows::Foundation::Numerics
namespace WFN = ABI::Windows::Foundation::Numerics;

using Handedness =
    ABI::Windows::UI::Input::Spatial::SpatialInteractionSourceHandedness;
using PressKind = ABI::Windows::UI::Input::Spatial::SpatialInteractionPressKind;
using SourceKind =
    ABI::Windows::UI::Input::Spatial::SpatialInteractionSourceKind;

using ABI::Windows::Foundation::IReference;
using ABI::Windows::Foundation::ITypedEventHandler;
using ABI::Windows::Foundation::Collections::IVectorView;
using ABI::Windows::Perception::IPerceptionTimestamp;
using ABI::Windows::Perception::People::IHeadPose;
using ABI::Windows::Perception::Spatial::ISpatialCoordinateSystem;
using ABI::Windows::UI::Input::Spatial::ISpatialInteractionManager;
using ABI::Windows::UI::Input::Spatial::ISpatialInteractionSource;
using ABI::Windows::UI::Input::Spatial::ISpatialInteractionSource2;
using ABI::Windows::UI::Input::Spatial::ISpatialInteractionSource3;
using ABI::Windows::UI::Input::Spatial::ISpatialInteractionSourceEventArgs;
using ABI::Windows::UI::Input::Spatial::ISpatialInteractionSourceEventArgs2;
using ABI::Windows::UI::Input::Spatial::ISpatialInteractionSourceLocation;
using ABI::Windows::UI::Input::Spatial::ISpatialInteractionSourceLocation2;
using ABI::Windows::UI::Input::Spatial::ISpatialInteractionSourceLocation3;
using ABI::Windows::UI::Input::Spatial::ISpatialInteractionSourceProperties;
using ABI::Windows::UI::Input::Spatial::ISpatialInteractionSourceState;
using ABI::Windows::UI::Input::Spatial::ISpatialPointerInteractionSourcePose;
using ABI::Windows::UI::Input::Spatial::ISpatialPointerInteractionSourcePose2;
using ABI::Windows::UI::Input::Spatial::ISpatialPointerPose;
using ABI::Windows::UI::Input::Spatial::ISpatialPointerPose2;
using ABI::Windows::UI::Input::Spatial::SpatialInteractionManager;
using ABI::Windows::UI::Input::Spatial::SpatialInteractionSourceEventArgs;
using ABI::Windows::UI::Input::Spatial::SpatialInteractionSourceState;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

typedef ITypedEventHandler<SpatialInteractionManager*,
                           SpatialInteractionSourceEventArgs*>
    SpatialInteractionSourceEventHandler;

namespace {
gfx::Transform CreateTransform(WFN::Vector3 position,
                               WFN::Quaternion rotation) {
  gfx::DecomposedTransform decomposed_transform;
  decomposed_transform.translate[0] = position.X;
  decomposed_transform.translate[1] = position.Y;
  decomposed_transform.translate[2] = position.Z;

  decomposed_transform.quaternion =
      gfx::Quaternion(rotation.X, rotation.Y, rotation.Z, rotation.W);
  return gfx::ComposeTransform(decomposed_transform);
}

bool TryGetGripFromLocation(
    ComPtr<ISpatialInteractionSourceLocation> location_in_origin,
    gfx::Transform* origin_from_grip) {
  DCHECK(origin_from_grip);
  *origin_from_grip = gfx::Transform();

  if (!location_in_origin)
    return false;

  ComPtr<IReference<WFN::Vector3>> pos_ref;
  HRESULT hr = location_in_origin->get_Position(&pos_ref);
  DCHECK(SUCCEEDED(hr));

  if (!pos_ref)
    return false;

  WFN::Vector3 pos;
  hr = pos_ref->get_Value(&pos);
  DCHECK(SUCCEEDED(hr));

  ComPtr<ISpatialInteractionSourceLocation2> location_in_origin2;
  hr = location_in_origin.As(&location_in_origin2);
  if (FAILED(hr))
    return false;

  ComPtr<IReference<WFN::Quaternion>> quat_ref;
  hr = location_in_origin2->get_Orientation(&quat_ref);
  DCHECK(SUCCEEDED(hr));

  if (!quat_ref)
    return false;

  WFN::Quaternion quat;
  hr = quat_ref->get_Value(&quat);
  DCHECK(SUCCEEDED(hr));

  *origin_from_grip = CreateTransform(pos, quat);
  return true;
}

bool TryGetPointerOffset(ComPtr<ISpatialInteractionSourceState> state,
                         ComPtr<ISpatialInteractionSource> source,
                         ComPtr<ISpatialCoordinateSystem> origin,
                         gfx::Transform origin_from_grip,
                         gfx::Transform* grip_from_pointer) {
  DCHECK(grip_from_pointer);
  *grip_from_pointer = gfx::Transform();

  if (!state || !source || !origin)
    return false;

  // We can get the pointer position, but we'll need to transform it to an
  // offset from the grip position.  If we can't get an inverse of that,
  // then go ahead and bail early.
  gfx::Transform grip_from_origin;
  if (!origin_from_grip.GetInverse(&grip_from_origin))
    return false;

  ComPtr<ISpatialInteractionSource2> source2;
  HRESULT hr = source.As(&source2);
  if (FAILED(hr))
    return false;

  boolean pointing_supported = false;
  hr = source2->get_IsPointingSupported(&pointing_supported);
  DCHECK(SUCCEEDED(hr));

  ComPtr<ISpatialPointerPose> pointer_pose;
  hr = state->TryGetPointerPose(origin.Get(), &pointer_pose);
  if (FAILED(hr) || !pointer_pose)
    return false;

  WFN::Vector3 pos;
  WFN::Quaternion rot;
  if (pointing_supported) {
    ComPtr<ISpatialPointerPose2> pointer_pose2;
    hr = pointer_pose.As(&pointer_pose2);
    if (FAILED(hr))
      return false;

    ComPtr<ISpatialPointerInteractionSourcePose> pointer_source_pose;
    hr = pointer_pose2->TryGetInteractionSourcePose(source.Get(),
                                                    &pointer_source_pose);
    if (FAILED(hr) || !pointer_source_pose)
      return false;

    hr = pointer_source_pose->get_Position(&pos);
    DCHECK(SUCCEEDED(hr));

    ComPtr<ISpatialPointerInteractionSourcePose2> pointer_source_pose2;
    hr = pointer_source_pose.As(&pointer_source_pose2);
    if (FAILED(hr))
      return false;

    hr = pointer_source_pose2->get_Orientation(&rot);
    DCHECK(SUCCEEDED(hr));
  } else {
    ComPtr<IHeadPose> head;
    hr = pointer_pose->get_Head(&head);
    DCHECK(SUCCEEDED(hr));

    hr = head->get_ForwardDirection(&pos);
    DCHECK(SUCCEEDED(hr));
  }

  gfx::Transform origin_from_pointer = CreateTransform(pos, rot);
  *grip_from_pointer = (grip_from_origin * origin_from_pointer);
  return true;
}

device::mojom::XRHandedness WindowsToMojoHandedness(Handedness handedness) {
  switch (handedness) {
    case Handedness::SpatialInteractionSourceHandedness_Left:
      return device::mojom::XRHandedness::LEFT;
    case Handedness::SpatialInteractionSourceHandedness_Right:
      return device::mojom::XRHandedness::RIGHT;
    default:
      return device::mojom::XRHandedness::NONE;
  }
}

uint32_t GetSourceId(ComPtr<ISpatialInteractionSource> source) {
  uint32_t id;
  HRESULT hr = source->get_Id(&id);
  DCHECK(SUCCEEDED(hr));

  // Voice's ID seems to be coming through as 0, which will cause a DCHECK in
  // the hash table used on the blink side.  To ensure that we don't have any
  // collisions with other ids, increment all of the ids by one.
  id++;
  DCHECK(id != 0);

  return id;
}
}  // namespace

MixedRealityInputHelper::MixedRealityInputHelper(HWND hwnd) : hwnd_(hwnd) {
  pressed_token_.value = 0;
  released_token_.value = 0;
}

MixedRealityInputHelper::~MixedRealityInputHelper() {
  // Dispose must be called before destruction, which ensures that we're
  // unsubscribed from events.
  DCHECK(pressed_token_.value == 0);
  DCHECK(released_token_.value == 0);
}

void MixedRealityInputHelper::Dispose() {
  UnsubscribeEvents();
}

bool MixedRealityInputHelper::EnsureSpatialInteractionManager() {
  if (spatial_interaction_manager_)
    return true;

  if (!hwnd_)
    return false;

  ComPtr<ISpatialInteractionManagerInterop> spatial_interaction_interop;
  base::win::ScopedHString spatial_interaction_interop_string =
      base::win::ScopedHString::Create(
          RuntimeClass_Windows_UI_Input_Spatial_SpatialInteractionManager);
  HRESULT hr = base::win::RoGetActivationFactory(
      spatial_interaction_interop_string.get(),
      IID_PPV_ARGS(&spatial_interaction_interop));
  if (FAILED(hr))
    return false;

  hr = spatial_interaction_interop->GetForWindow(
      hwnd_, IID_PPV_ARGS(&spatial_interaction_manager_));
  if (FAILED(hr))
    return false;

  SubscribeEvents();
  return true;
}

std::vector<mojom::XRInputSourceStatePtr>
MixedRealityInputHelper::GetInputState(ComPtr<ISpatialCoordinateSystem> origin,
                                       ComPtr<IPerceptionTimestamp> timestamp) {
  std::vector<mojom::XRInputSourceStatePtr> input_states;

  if (!timestamp || !origin || !EnsureSpatialInteractionManager())
    return input_states;

  ComPtr<IVectorView<SpatialInteractionSourceState*>> source_states;
  if (FAILED(spatial_interaction_manager_->GetDetectedSourcesAtTimestamp(
          timestamp.Get(), &source_states)))
    return input_states;

  unsigned int size;
  HRESULT hr = source_states->get_Size(&size);
  DCHECK(SUCCEEDED(hr));

  base::AutoLock scoped_lock(lock_);

  for (unsigned int i = 0; i < size; i++) {
    ComPtr<ISpatialInteractionSourceState> state;
    if (FAILED(source_states->GetAt(i, &state)))
      continue;

    mojom::XRInputSourceStatePtr source_state =
        LockedParseWindowsSourceState(state, origin);

    if (source_state)
      input_states.push_back(std::move(source_state));
  }

  for (unsigned int i = 0; i < pending_voice_states_.size(); i++) {
    mojom::XRInputSourceStatePtr source_state =
        LockedParseWindowsSourceState(pending_voice_states_[i], origin);

    if (source_state)
      input_states.push_back(std::move(source_state));
  }

  pending_voice_states_.clear();

  return input_states;
}

mojom::XRInputSourceStatePtr
MixedRealityInputHelper::LockedParseWindowsSourceState(
    ComPtr<ISpatialInteractionSourceState> state,
    ComPtr<ISpatialCoordinateSystem> origin) {
  if (!(state && origin))
    return nullptr;

  ComPtr<ISpatialInteractionSource> source;
  HRESULT hr = state->get_Source(&source);
  DCHECK(SUCCEEDED(hr));

  SourceKind source_kind;
  hr = source->get_Kind(&source_kind);
  DCHECK(SUCCEEDED(hr));

  bool is_controller =
      (source_kind == SourceKind::SpatialInteractionSourceKind_Controller);
  bool is_voice =
      (source_kind == SourceKind::SpatialInteractionSourceKind_Voice);

  if (!(is_controller || is_voice))
    return nullptr;

  // Note that if this is from voice input, we're not supposed to send up the
  // "grip" position, so this will be left as identity and let us still use
  // the same code paths, as any transformations by it will leave the original
  // item unaffected.
  gfx::Transform origin_from_grip;
  if (is_controller) {
    ComPtr<ISpatialInteractionSourceProperties> props;
    hr = state->get_Properties(&props);
    DCHECK(SUCCEEDED(hr));
    ComPtr<ISpatialInteractionSourceLocation> location_in_origin;
    if (FAILED(props->TryGetLocation(origin.Get(), &location_in_origin)) ||
        !location_in_origin)
      return nullptr;

    if (!TryGetGripFromLocation(location_in_origin, &origin_from_grip))
      return nullptr;
  }

  gfx::Transform grip_from_pointer;
  if (!TryGetPointerOffset(state, source, origin, origin_from_grip,
                           &grip_from_pointer))
    return nullptr;

  // Now that we know we have tracking for the object, we'll start building.
  device::mojom::XRInputSourceStatePtr source_state =
      device::mojom::XRInputSourceState::New();

  // Hands may not have the same id especially if they are lost but since we
  // are only tracking controllers/voice, this id should be consistent.
  uint32_t id = GetSourceId(source);

  source_state->source_id = id;
  source_state->primary_input_pressed = controller_states_[id].pressed;
  source_state->primary_input_clicked = controller_states_[id].clicked;
  controller_states_[id].clicked = false;

  // Grip position should *only* be specified for a controller.
  if (is_controller) {
    source_state->grip = origin_from_grip;
  }

  device::mojom::XRInputSourceDescriptionPtr description =
      device::mojom::XRInputSourceDescription::New();

  // If we've gotten this far we've gotten the real position.
  description->emulated_position = false;
  description->pointer_offset = grip_from_pointer;

  if (is_voice) {
    description->target_ray_mode = device::mojom::XRTargetRayMode::GAZING;
    description->handedness = device::mojom::XRHandedness::NONE;
  } else if (is_controller) {
    description->target_ray_mode = device::mojom::XRTargetRayMode::POINTING;

    ComPtr<ISpatialInteractionSource3> source3;
    hr = source.As(&source3);
    if (FAILED(hr))
      return nullptr;

    Handedness handedness;
    hr = source3->get_Handedness(&handedness);
    DCHECK(SUCCEEDED(hr));

    description->handedness = WindowsToMojoHandedness(handedness);
  } else {
    NOTREACHED();
  }

  source_state->description = std::move(description);

  return source_state;
}

HRESULT MixedRealityInputHelper::OnSourcePressed(
    ISpatialInteractionManager* sender,
    ISpatialInteractionSourceEventArgs* args) {
  return ProcessSourceEvent(args, true /* is_pressed */);
}

HRESULT MixedRealityInputHelper::OnSourceReleased(
    ISpatialInteractionManager* sender,
    ISpatialInteractionSourceEventArgs* args) {
  return ProcessSourceEvent(args, false /* is_pressed */);
}

HRESULT MixedRealityInputHelper::ProcessSourceEvent(
    ISpatialInteractionSourceEventArgs* raw_args,
    bool is_pressed) {
  base::AutoLock scoped_lock(lock_);
  ComPtr<ISpatialInteractionSourceEventArgs> args(raw_args);
  ComPtr<ISpatialInteractionSourceEventArgs2> args2;
  HRESULT hr = args.As(&args2);
  if (FAILED(hr))
    return S_OK;

  PressKind press_kind;
  hr = args2->get_PressKind(&press_kind);
  DCHECK(SUCCEEDED(hr));

  if (press_kind != PressKind::SpatialInteractionPressKind_Select)
    return S_OK;

  ComPtr<ISpatialInteractionSourceState> state;
  hr = args->get_State(&state);
  DCHECK(SUCCEEDED(hr));

  ComPtr<ISpatialInteractionSource> source;
  hr = state->get_Source(&source);
  DCHECK(SUCCEEDED(hr));

  SourceKind source_kind;
  hr = source->get_Kind(&source_kind);
  DCHECK(SUCCEEDED(hr));

  if (source_kind != SourceKind::SpatialInteractionSourceKind_Controller &&
      source_kind != SourceKind::SpatialInteractionSourceKind_Voice)
    return S_OK;

  uint32_t id = GetSourceId(source);

  bool wasPressed = controller_states_[id].pressed;
  bool wasClicked = controller_states_[id].clicked;
  controller_states_[id].pressed = is_pressed;
  controller_states_[id].clicked = wasClicked || (wasPressed && !is_pressed);

  // Tracked controllers show up when we poll for DetectedSources, but voice
  // does not.
  if (source_kind == SourceKind::SpatialInteractionSourceKind_Voice &&
      !is_pressed)
    pending_voice_states_.push_back(state);
  return S_OK;
}

void MixedRealityInputHelper::SubscribeEvents() {
  DCHECK(spatial_interaction_manager_);
  DCHECK(pressed_token_.value == 0);
  DCHECK(released_token_.value == 0);

  // The destructor ensures that we're unsubscribed so raw this is fine.
  auto pressed_callback = Callback<SpatialInteractionSourceEventHandler>(
      this, &MixedRealityInputHelper::OnSourcePressed);
  HRESULT hr = spatial_interaction_manager_->add_SourcePressed(
      pressed_callback.Get(), &pressed_token_);
  DCHECK(SUCCEEDED(hr));

  // The destructor ensures that we're unsubscribed so raw this is fine.
  auto released_callback = Callback<SpatialInteractionSourceEventHandler>(
      this, &MixedRealityInputHelper::OnSourceReleased);
  hr = spatial_interaction_manager_->add_SourceReleased(released_callback.Get(),
                                                        &released_token_);
  DCHECK(SUCCEEDED(hr));
}

void MixedRealityInputHelper::UnsubscribeEvents() {
  base::AutoLock scoped_lock(lock_);
  if (!spatial_interaction_manager_)
    return;

  HRESULT hr = S_OK;
  if (pressed_token_.value != 0) {
    hr = spatial_interaction_manager_->remove_SourcePressed(pressed_token_);
    pressed_token_.value = 0;
    DCHECK(SUCCEEDED(hr));
  }

  if (released_token_.value != 0) {
    hr = spatial_interaction_manager_->remove_SourceReleased(released_token_);
    released_token_.value = 0;
    DCHECK(SUCCEEDED(hr));
  }
}

}  // namespace device
