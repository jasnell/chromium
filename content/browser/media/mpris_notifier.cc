// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/media/mpris_notifier.h"

#include <memory>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "services/media_session/public/mojom/constants.mojom.h"
#include "services/media_session/public/mojom/media_session.mojom.h"
#include "services/service_manager/public/cpp/connector.h"
#include "ui/base/mpris/mpris_service.h"

namespace content {

MprisNotifier::MprisNotifier(service_manager::Connector* connector)
    : connector_(connector) {}

MprisNotifier::~MprisNotifier() = default;

void MprisNotifier::Initialize() {
  // |service_| can be set in tests.
  if (!service_)
    service_ = mpris::MprisService::GetInstance();
  DCHECK(service_);

  // |connector_| can be null in tests.
  if (!connector_)
    return;

  // Connect to the MediaControllerManager and create a MediaController that
  // controls the active session so we can observe it.
  media_session::mojom::MediaControllerManagerPtr controller_manager_ptr;
  connector_->BindInterface(media_session::mojom::kServiceName,
                            mojo::MakeRequest(&controller_manager_ptr));
  controller_manager_ptr->CreateActiveMediaController(
      mojo::MakeRequest(&media_controller_ptr_));

  // Observe the active media controller for changes to playback state and
  // supported actions.
  media_session::mojom::MediaControllerObserverPtr media_controller_observer;
  media_controller_observer_binding_.Bind(
      mojo::MakeRequest(&media_controller_observer));
  media_controller_ptr_->AddObserver(std::move(media_controller_observer));
}

void MprisNotifier::MediaSessionInfoChanged(
    media_session::mojom::MediaSessionInfoPtr session_info) {
  DCHECK(service_);

  session_info_ = std::move(session_info);
  if (session_info_) {
    if (session_info_->playback_state ==
        media_session::mojom::MediaPlaybackState::kPlaying) {
      service_->SetPlaybackStatus(
          mpris::MprisService::PlaybackStatus::kPlaying);
    } else {
      service_->SetPlaybackStatus(mpris::MprisService::PlaybackStatus::kPaused);
    }
  } else {
    service_->SetPlaybackStatus(mpris::MprisService::PlaybackStatus::kStopped);
  }
}

void MprisNotifier::MediaSessionMetadataChanged(
    const base::Optional<media_session::MediaMetadata>& metadata) {
  // TODO(https://crbug.com/952410): Set metadata on |service_|.
}

}  // namespace content
