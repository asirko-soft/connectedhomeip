/*
 *
 *    Copyright (c) 2025 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <app-common/zap-generated/cluster-enums.h>
#include <app/CASESessionManager.h>
#include <app/clusters/webrtc-transport-provider-server/webrtc-transport-provider-server.h>
#include <rtc/rtc.hpp>

namespace Camera {

class WebRTCProviderManager : public chip::app::Clusters::WebRTCTransportProvider::Delegate
{
public:
    WebRTCProviderManager() :
        mOnConnectedCallback(OnDeviceConnected, this), mOnConnectionFailureCallback(OnDeviceConnectionFailure, this)
    {}

    ~WebRTCProviderManager() { CloseConnection(); };

    void Init();

    void CloseConnection();

    CHIP_ERROR HandleSolicitOffer(const OfferRequestArgs & args,
                                  chip::app::Clusters::WebRTCTransportProvider::WebRTCSessionStruct & outSession,
                                  bool & outDeferredOffer) override;

    CHIP_ERROR
    HandleProvideOffer(const ProvideOfferRequestArgs & args,
                       chip::app::Clusters::WebRTCTransportProvider::WebRTCSessionStruct & outSession) override;

    CHIP_ERROR HandleProvideAnswer(uint16_t sessionId, const std::string & sdpAnswer) override;

    CHIP_ERROR HandleProvideICECandidates(uint16_t sessionId, const std::vector<std::string> & candidates) override;

    CHIP_ERROR HandleEndSession(uint16_t sessionId, chip::app::Clusters::WebRTCTransportProvider::WebRTCEndReasonEnum reasonCode,
                                chip::app::DataModel::Nullable<uint16_t> videoStreamID,
                                chip::app::DataModel::Nullable<uint16_t> audioStreamID) override;

    CHIP_ERROR ValidateStreamUsage(chip::app::Clusters::WebRTCTransportProvider::StreamUsageEnum streamUsage,
                                   const chip::Optional<chip::app::DataModel::Nullable<uint16_t>> & videoStreamId,
                                   const chip::Optional<chip::app::DataModel::Nullable<uint16_t>> & audioStreamId) override;

private:
    enum class CommandType : uint8_t
    {
        kUndefined = 0,
        kAnswer    = 1,
    };

    void ScheduleAnswerSend();

    CHIP_ERROR SendAnswerCommand(chip::Messaging::ExchangeManager & exchangeMgr, const chip::SessionHandle & sessionHandle);

    static void OnDeviceConnected(void * context, chip::Messaging::ExchangeManager & exchangeMgr,
                                  const chip::SessionHandle & sessionHandle);

    static void OnDeviceConnectionFailure(void * context, const chip::ScopedNodeId & peerId, CHIP_ERROR error);

    std::shared_ptr<rtc::PeerConnection> mPeerConnection;
    std::shared_ptr<rtc::DataChannel> mDataChannel;

    chip::ScopedNodeId mPeerId;
    chip::EndpointId mOriginatingEndpointId;

    CommandType mCommandType = CommandType::kUndefined;

    uint16_t mCurrentSessionId = 0;
    std::string mSdpAnswer;

    chip::Callback::Callback<chip::OnDeviceConnected> mOnConnectedCallback;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> mOnConnectionFailureCallback;
};

} // namespace Camera
