/*
 *
 *    Copyright (c) 2020-2021 Project CHIP Authors
 *    Copyright (c) 2014-2017 Nest Labs, Inc.
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

/**
 *    @file
 *      This file implements objects which provide an abstraction layer between
 *      a platform's Bluetooth Low Energy (BLE) implementation and the CHIP
 *      stack.
 *
 *      The BleLayer object accepts BLE data and control input from the
 *      application via a functional interface. It performs the fragmentation
 *      and reassembly required to transmit CHIP message via a BLE GATT
 *      characteristic interface, and drives incoming messages up the CHIP
 *      stack.
 *
 *      During initialization, the BleLayer object requires a pointer to the
 *      platform's implementation of the BlePlatformDelegate and
 *      BleApplicationDelegate objects.
 *
 *      The BlePlatformDelegate provides the CHIP stack with an interface
 *      by which to form and cancel GATT subscriptions, read and write
 *      GATT characteristic values, send GATT characteristic notifications,
 *      respond to GATT read requests, and close BLE connections.
 *
 *      The BleApplicationDelegate provides a mechanism for CHIP to inform
 *      the application when it has finished using a given BLE connection,
 *      i.e when the chipConnection object wrapping this connection has
 *      closed. This allows the application to either close the BLE connection
 *      or continue to keep it open for non-CHIP purposes.
 *
 *      To enable CHIP over BLE for a new platform, the application developer
 *      must provide an implementation for both delegates, provides points to
 *      instances of these delegates on startup, and ensure that the
 *      application calls the necessary BleLayer functions when appropriate to
 *      drive BLE data and control input up the stack.
 */

#define _CHIP_BLE_BLE_H
#include "BleLayer.h"

#include <cstring>
#include <utility>

#include <lib/core/CHIPEncoding.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/SetupDiscriminator.h>
#include <lib/support/logging/CHIPLogging.h>
#include <system/SystemLayer.h>
#include <system/SystemPacketBuffer.h>

#include "BLEEndPoint.h"
#include "BleApplicationDelegate.h"
#include "BleConfig.h"
#include "BleConnectionDelegate.h"
#include "BleError.h"
#include "BleLayerDelegate.h"
#include "BlePlatformDelegate.h"
#include "BleRole.h"
#include "BleUUID.h"

// Magic values expected in first 2 bytes of valid BLE transport capabilities request or response:
#define CAPABILITIES_MSG_CHECK_BYTE_1 0b01100101
#define CAPABILITIES_MSG_CHECK_BYTE_2 0b01101100

namespace chip {
namespace Ble {

class BleEndPointPool
{
public:
    int Size() const { return BLE_LAYER_NUM_BLE_ENDPOINTS; }

    BLEEndPoint * Get(size_t i) const
    {
        static union
        {
            uint8_t Pool[sizeof(BLEEndPoint) * BLE_LAYER_NUM_BLE_ENDPOINTS];
            BLEEndPoint::AlignT ForceAlignment;
        } sEndPointPool;

        if (i < BLE_LAYER_NUM_BLE_ENDPOINTS)
        {
            return reinterpret_cast<BLEEndPoint *>(sEndPointPool.Pool + (sizeof(BLEEndPoint) * i));
        }

        return nullptr;
    }

    BLEEndPoint * Find(BLE_CONNECTION_OBJECT c) const
    {
        if (c == BLE_CONNECTION_UNINITIALIZED)
        {
            return nullptr;
        }

        for (size_t i = 0; i < BLE_LAYER_NUM_BLE_ENDPOINTS; i++)
        {
            BLEEndPoint * elem = Get(i);
            if (elem->mBle != nullptr && elem->mConnObj == c)
            {
                return elem;
            }
        }

        return nullptr;
    }

    BLEEndPoint * GetFree() const
    {
        for (size_t i = 0; i < BLE_LAYER_NUM_BLE_ENDPOINTS; i++)
        {
            BLEEndPoint * elem = Get(i);
            if (elem->mBle == nullptr)
            {
                return elem;
            }
        }
        return nullptr;
    }
};

// EndPoint Pools
//
static BleEndPointPool sBLEEndPointPool;

// BleTransportCapabilitiesRequestMessage implementation:

void BleTransportCapabilitiesRequestMessage::SetSupportedProtocolVersion(uint8_t index, uint8_t version)
{
    uint8_t mask;

    // If even-index, store version in lower 4 bits; else, higher 4 bits.
    if (index % 2 == 0)
    {
        mask = 0x0F;
    }
    else
    {
        mask    = 0xF0;
        version = static_cast<uint8_t>(version << 4);
    }

    version &= mask;

    uint8_t & slot = mSupportedProtocolVersions[(index / 2)];
    slot           = static_cast<uint8_t>(slot & ~mask); // Clear version at index; leave other version in same byte alone
    slot |= version;
}

CHIP_ERROR BleTransportCapabilitiesRequestMessage::Encode(const PacketBufferHandle & msgBuf) const
{
    uint8_t * p = msgBuf->Start();

    // Verify we can write the fixed-length request without running into the end of the buffer.
    VerifyOrReturnError(msgBuf->MaxDataLength() >= kCapabilitiesRequestLength, CHIP_ERROR_NO_MEMORY);

    chip::Encoding::Write8(p, CAPABILITIES_MSG_CHECK_BYTE_1);
    chip::Encoding::Write8(p, CAPABILITIES_MSG_CHECK_BYTE_2);

    for (uint8_t version : mSupportedProtocolVersions)
    {
        chip::Encoding::Write8(p, version);
    }

    chip::Encoding::LittleEndian::Write16(p, mMtu);
    chip::Encoding::Write8(p, mWindowSize);

    msgBuf->SetDataLength(kCapabilitiesRequestLength);

    return CHIP_NO_ERROR;
}

CHIP_ERROR BleTransportCapabilitiesRequestMessage::Decode(const PacketBufferHandle & msgBuf,
                                                          BleTransportCapabilitiesRequestMessage & msg)
{
    const uint8_t * p = msgBuf->Start();

    // Verify we can read the fixed-length request without running into the end of the buffer.
    VerifyOrReturnError(msgBuf->DataLength() >= kCapabilitiesRequestLength, CHIP_ERROR_MESSAGE_INCOMPLETE);

    VerifyOrReturnError(CAPABILITIES_MSG_CHECK_BYTE_1 == chip::Encoding::Read8(p), BLE_ERROR_INVALID_MESSAGE);
    VerifyOrReturnError(CAPABILITIES_MSG_CHECK_BYTE_2 == chip::Encoding::Read8(p), BLE_ERROR_INVALID_MESSAGE);

    static_assert(kCapabilitiesRequestSupportedVersionsLength == sizeof(msg.mSupportedProtocolVersions),
                  "Expected capability sizes and storage must match");
    for (unsigned char & version : msg.mSupportedProtocolVersions)
    {
        version = chip::Encoding::Read8(p);
    }

    msg.mMtu        = chip::Encoding::LittleEndian::Read16(p);
    msg.mWindowSize = chip::Encoding::Read8(p);

    return CHIP_NO_ERROR;
}

// BleTransportCapabilitiesResponseMessage implementation:

CHIP_ERROR BleTransportCapabilitiesResponseMessage::Encode(const PacketBufferHandle & msgBuf) const
{
    uint8_t * p = msgBuf->Start();

    // Verify we can write the fixed-length request without running into the end of the buffer.
    VerifyOrReturnError(msgBuf->MaxDataLength() >= kCapabilitiesResponseLength, CHIP_ERROR_NO_MEMORY);

    chip::Encoding::Write8(p, CAPABILITIES_MSG_CHECK_BYTE_1);
    chip::Encoding::Write8(p, CAPABILITIES_MSG_CHECK_BYTE_2);

    chip::Encoding::Write8(p, mSelectedProtocolVersion);
    chip::Encoding::LittleEndian::Write16(p, mFragmentSize);
    chip::Encoding::Write8(p, mWindowSize);

    msgBuf->SetDataLength(kCapabilitiesResponseLength);

    return CHIP_NO_ERROR;
}

CHIP_ERROR BleTransportCapabilitiesResponseMessage::Decode(const PacketBufferHandle & msgBuf,
                                                           BleTransportCapabilitiesResponseMessage & msg)
{
    const uint8_t * p = msgBuf->Start();

    // Verify we can read the fixed-length response without running into the end of the buffer.
    VerifyOrReturnError(msgBuf->DataLength() >= kCapabilitiesResponseLength, CHIP_ERROR_MESSAGE_INCOMPLETE);

    VerifyOrReturnError(CAPABILITIES_MSG_CHECK_BYTE_1 == chip::Encoding::Read8(p), BLE_ERROR_INVALID_MESSAGE);
    VerifyOrReturnError(CAPABILITIES_MSG_CHECK_BYTE_2 == chip::Encoding::Read8(p), BLE_ERROR_INVALID_MESSAGE);

    msg.mSelectedProtocolVersion = chip::Encoding::Read8(p);
    msg.mFragmentSize            = chip::Encoding::LittleEndian::Read16(p);
    msg.mWindowSize              = chip::Encoding::Read8(p);

    return CHIP_NO_ERROR;
}

// BleLayer implementation:

BleLayer::BleLayer()
{
    mState = kState_NotInitialized;
}

CHIP_ERROR BleLayer::Init(BlePlatformDelegate * platformDelegate, BleConnectionDelegate * connDelegate,
                          BleApplicationDelegate * appDelegate, chip::System::Layer * systemLayer)
{
    Ble::RegisterLayerErrorFormatter();

    // It is totally valid to not have a connDelegate. In this case the client application
    // will take care of the connection steps.
    VerifyOrReturnError(platformDelegate != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(appDelegate != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(systemLayer != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    if (mState != kState_NotInitialized)
    {
        return CHIP_ERROR_INCORRECT_STATE;
    }

    mConnectionDelegate  = connDelegate;
    mPlatformDelegate    = platformDelegate;
    mApplicationDelegate = appDelegate;
    mSystemLayer         = systemLayer;

    memset(&sBLEEndPointPool, 0, sizeof(sBLEEndPointPool));

    mState = kState_Initialized;

    return CHIP_NO_ERROR;
}

CHIP_ERROR BleLayer::Init(BlePlatformDelegate * platformDelegate, BleApplicationDelegate * appDelegate,
                          chip::System::Layer * systemLayer)
{
    return Init(platformDelegate, nullptr, appDelegate, systemLayer);
}

void BleLayer::IndicateBleClosing()
{
    mState = kState_Disconnecting;
}

void BleLayer::Shutdown()
{
    mState = kState_NotInitialized;
    CloseAllBleConnections();
}

void BleLayer::CloseAllBleConnections()
{
    // Close and free all BLE end points.
    for (size_t i = 0; i < BLE_LAYER_NUM_BLE_ENDPOINTS; i++)
    {
        BLEEndPoint * elem = sBLEEndPointPool.Get(i);

        // If end point was initialized, and has not since been freed...
        if (elem->mBle != nullptr)
        {
            // If end point hasn't already been closed...
            if (elem->mState != BLEEndPoint::kState_Closed)
            {
                // Close end point such that callbacks are suppressed and pending transmissions aborted.
                elem->Abort();
            }

            // If end point was closed, but is still waiting for GATT unsubscribe to complete, free it anyway.
            // This cancels the unsubscribe timer (plus all the end point's other timers).
            if (elem->IsUnsubscribePending())
            {
                elem->Free();
            }
        }
    }
}

void BleLayer::CloseBleConnection(BLE_CONNECTION_OBJECT connObj)
{
    // Close and free all BLE endpoints.
    for (size_t i = 0; i < BLE_LAYER_NUM_BLE_ENDPOINTS; i++)
    {
        BLEEndPoint * elem = sBLEEndPointPool.Get(i);

        // If end point was initialized, and has not since been freed...
        if (elem->mBle != nullptr && elem->ConnectionObjectIs(connObj))
        {
            // If end point hasn't already been closed...
            if (elem->mState != BLEEndPoint::kState_Closed)
            {
                // Close end point such that callbacks are suppressed and pending transmissions aborted.
                elem->Abort();
            }

            // If end point was closed, but is still waiting for GATT unsubscribe to complete, free it anyway.
            // This cancels the unsubscribe timer (plus all the end point's other timers).
            if (elem->IsUnsubscribePending())
            {
                elem->Free();
            }
        }
    }
}

CHIP_ERROR BleLayer::CancelBleIncompleteConnection()
{
    VerifyOrReturnError(mState == kState_Initialized, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mConnectionDelegate != nullptr, CHIP_ERROR_INCORRECT_STATE);

    CHIP_ERROR err = mConnectionDelegate->CancelConnection();
    if (err == CHIP_ERROR_NOT_IMPLEMENTED)
    {
        ChipLogError(Ble, "BleConnectionDelegate::CancelConnection is not implemented.");
    }
    return err;
}

CHIP_ERROR BleLayer::NewBleConnectionByDiscriminator(const SetupDiscriminator & connDiscriminator, void * appState,
                                                     BleConnectionDelegate::OnConnectionCompleteFunct onSuccess,
                                                     BleConnectionDelegate::OnConnectionErrorFunct onError)
{
    VerifyOrReturnError(mState == kState_Initialized, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mConnectionDelegate != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mBleTransport != nullptr, CHIP_ERROR_INCORRECT_STATE);

    mConnectionDelegate->OnConnectionComplete = onSuccess;
    mConnectionDelegate->OnConnectionError    = onError;

    mConnectionDelegate->NewConnection(this, appState == nullptr ? this : appState, connDiscriminator);

    return CHIP_NO_ERROR;
}

CHIP_ERROR BleLayer::NewBleConnectionByObject(BLE_CONNECTION_OBJECT connObj, void * appState,
                                              BleConnectionDelegate::OnConnectionCompleteFunct onSuccess,
                                              BleConnectionDelegate::OnConnectionErrorFunct onError)
{
    VerifyOrReturnError(mState == kState_Initialized, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mConnectionDelegate != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mBleTransport != nullptr, CHIP_ERROR_INCORRECT_STATE);

    mConnectionDelegate->OnConnectionComplete = onSuccess;
    mConnectionDelegate->OnConnectionError    = onError;

    mConnectionDelegate->NewConnection(this, appState == nullptr ? this : appState, connObj);

    return CHIP_NO_ERROR;
}

CHIP_ERROR BleLayer::NewBleConnectionByObject(BLE_CONNECTION_OBJECT connObj)
{
    VerifyOrReturnError(mState == kState_Initialized, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mBleTransport != nullptr, CHIP_ERROR_INCORRECT_STATE);

    OnConnectionComplete(this, connObj);

    return CHIP_NO_ERROR;
}

CHIP_ERROR BleLayer::NewBleConnectionByDiscriminators(const Span<const SetupDiscriminator> & discriminators, void * appState,
                                                      BleConnectionDelegate::OnConnectionByDiscriminatorsCompleteFunct onSuccess,
                                                      BleConnectionDelegate::OnConnectionErrorFunct onError)
{
    VerifyOrReturnError(mState == kState_Initialized, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mConnectionDelegate != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mBleTransport != nullptr, CHIP_ERROR_INCORRECT_STATE);

    return mConnectionDelegate->NewConnection(this, appState, discriminators, onSuccess, onError);
}

CHIP_ERROR BleLayer::NewBleEndPoint(BLEEndPoint ** retEndPoint, BLE_CONNECTION_OBJECT connObj, BleRole role, bool autoClose)
{
    *retEndPoint = nullptr;

    if (mState != kState_Initialized)
    {
        return CHIP_ERROR_INCORRECT_STATE;
    }

    if (connObj == BLE_CONNECTION_UNINITIALIZED)
    {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    *retEndPoint = sBLEEndPointPool.GetFree();
    if (*retEndPoint == nullptr)
    {
        ChipLogError(Ble, "%s endpoint pool FULL", "Ble");
        return CHIP_ERROR_ENDPOINT_POOL_FULL;
    }

    (*retEndPoint)->Init(this, connObj, role, autoClose);
    (*retEndPoint)->mBleTransport = mBleTransport;

    return CHIP_NO_ERROR;
}

// Handle remote central's initiation of CHIP over BLE protocol handshake.
CHIP_ERROR BleLayer::HandleBleTransportConnectionInitiated(BLE_CONNECTION_OBJECT connObj, PacketBufferHandle && pBuf)
{
    CHIP_ERROR err            = CHIP_NO_ERROR;
    BLEEndPoint * newEndPoint = nullptr;

    // Only BLE peripherals can receive GATT writes, so specify this role in our creation of the BLEEndPoint.
    // Set autoClose = false. Peripherals only notify the application when an end point releases a BLE connection.
    err = NewBleEndPoint(&newEndPoint, connObj, kBleRole_Peripheral, false);
    SuccessOrExit(err);

    newEndPoint->mBleTransport = mBleTransport;

    err = newEndPoint->Receive(std::move(pBuf));
    SuccessOrExit(err); // If we fail here, end point will have already released connection and freed itself.

exit:
    // If we failed to allocate a new end point, release underlying BLE connection. Central's handshake will time out
    // if the application decides to keep the BLE connection open.
    if (newEndPoint == nullptr)
    {
        mApplicationDelegate->NotifyChipConnectionClosed(connObj);
    }

    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Ble, "HandleChipConnectionReceived failed, err = %" CHIP_ERROR_FORMAT, err.Format());
    }

    return err;
}

bool BleLayer::HandleWriteReceived(BLE_CONNECTION_OBJECT connObj, const ChipBleUUID * svcId, const ChipBleUUID * charId,
                                   PacketBufferHandle && pBuf)
{
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_SVC_ID, svcId), false, ChipLogError(Ble, "Write received on unknown svc"));
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_CHAR_1_UUID, charId), false, ChipLogError(Ble, "Write received on unknown char"));
    VerifyOrReturnError(!pBuf.IsNull(), false, ChipLogError(Ble, "Write received null buffer"));

    // Find matching connection end point.
    BLEEndPoint * endPoint = sBLEEndPointPool.Find(connObj);

    if (endPoint != nullptr)
    {
        CHIP_ERROR err = endPoint->Receive(std::move(pBuf));
        VerifyOrReturnError(err == CHIP_NO_ERROR, false,
                            ChipLogError(Ble, "Receive failed, err = %" CHIP_ERROR_FORMAT, err.Format()));
    }
    else
    {
        CHIP_ERROR err = HandleBleTransportConnectionInitiated(connObj, std::move(pBuf));
        VerifyOrReturnError(err == CHIP_NO_ERROR, false,
                            ChipLogError(Ble, "Handle new BLE connection failed, err = %" CHIP_ERROR_FORMAT, err.Format()));
    }

    return true;
}

bool BleLayer::HandleIndicationReceived(BLE_CONNECTION_OBJECT connObj, const ChipBleUUID * svcId, const ChipBleUUID * charId,
                                        PacketBufferHandle && pBuf)
{
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_SVC_ID, svcId), false, ChipLogError(Ble, "Indication received on unknown svc"));
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_CHAR_2_UUID, charId), false, ChipLogError(Ble, "Indication received on unknown char"));
    VerifyOrReturnError(!pBuf.IsNull(), false, ChipLogError(Ble, "Indication received null buffer"));

    // Find matching connection end point.
    BLEEndPoint * endPoint = sBLEEndPointPool.Find(connObj);
    VerifyOrReturnError(endPoint != nullptr, false, ChipLogDetail(Ble, "No endpoint for received indication"));

    CHIP_ERROR err = endPoint->Receive(std::move(pBuf));
    VerifyOrReturnError(err == CHIP_NO_ERROR, false, ChipLogError(Ble, "Receive failed, err = %" CHIP_ERROR_FORMAT, err.Format()));

    return true;
}

bool BleLayer::HandleWriteConfirmation(BLE_CONNECTION_OBJECT connObj, const ChipBleUUID * svcId, const ChipBleUUID * charId)
{
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_SVC_ID, svcId), false, ChipLogError(Ble, "Write confirmation on unknown svc"));
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_CHAR_1_UUID, charId), false, ChipLogError(Ble, "Write confirmation on unknown char"));

    HandleAckReceived(connObj);
    return true;
}

bool BleLayer::HandleIndicationConfirmation(BLE_CONNECTION_OBJECT connObj, const ChipBleUUID * svcId, const ChipBleUUID * charId)
{
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_SVC_ID, svcId), false, ChipLogError(Ble, "Indication confirmation on unknown svc"));
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_CHAR_2_UUID, charId), false,
                        ChipLogError(Ble, "Indication confirmation on unknown char"));

    HandleAckReceived(connObj);
    return true;
}

void BleLayer::HandleAckReceived(BLE_CONNECTION_OBJECT connObj)
{
    // Find matching connection end point.
    BLEEndPoint * endPoint = sBLEEndPointPool.Find(connObj);
    VerifyOrReturn(endPoint != nullptr, ChipLogDetail(Ble, "No endpoint for received ack"));

    CHIP_ERROR err = endPoint->HandleGattSendConfirmationReceived();
    VerifyOrReturn(err == CHIP_NO_ERROR,
                   ChipLogError(Ble, "Send ack confirmation failed, err = %" CHIP_ERROR_FORMAT, err.Format()));
}

bool BleLayer::HandleSubscribeReceived(BLE_CONNECTION_OBJECT connObj, const ChipBleUUID * svcId, const ChipBleUUID * charId)
{
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_SVC_ID, svcId), false, ChipLogError(Ble, "Subscribe received on unknown svc"));
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_CHAR_2_UUID, charId) || UUIDsMatch(&CHIP_BLE_CHAR_3_UUID, charId), false,
                        ChipLogError(Ble, "Subscribe received on unknown char"));

    // Find end point already associated with BLE connection, if any.
    BLEEndPoint * endPoint = sBLEEndPointPool.Find(connObj);
    VerifyOrReturnError(endPoint != nullptr, false, ChipLogDetail(Ble, "No endpoint for received subscribe"));

    endPoint->HandleSubscribeReceived();
    return true;
}

bool BleLayer::HandleSubscribeComplete(BLE_CONNECTION_OBJECT connObj, const ChipBleUUID * svcId, const ChipBleUUID * charId)
{
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_SVC_ID, svcId), false, ChipLogError(Ble, "Subscribe complete on unknown svc"));
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_CHAR_2_UUID, charId) || UUIDsMatch(&CHIP_BLE_CHAR_3_UUID, charId), false,
                        ChipLogError(Ble, "Subscribe complete on unknown char"));

    BLEEndPoint * endPoint = sBLEEndPointPool.Find(connObj);
    VerifyOrReturnError(endPoint != nullptr, false, ChipLogDetail(Ble, "No endpoint for subscribe complete"));

    endPoint->HandleSubscribeComplete();
    return true;
}

bool BleLayer::HandleUnsubscribeReceived(BLE_CONNECTION_OBJECT connObj, const ChipBleUUID * svcId, const ChipBleUUID * charId)
{
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_SVC_ID, svcId), false, ChipLogError(Ble, "Unsubscribe received on unknown svc"));
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_CHAR_2_UUID, charId) || UUIDsMatch(&CHIP_BLE_CHAR_3_UUID, charId), false,
                        ChipLogError(Ble, "Unsubscribe received on unknown char"));

    // Find end point already associated with BLE connection, if any.
    BLEEndPoint * endPoint = sBLEEndPointPool.Find(connObj);
    VerifyOrReturnError(endPoint != nullptr, false, ChipLogDetail(Ble, "No endpoint for unsubscribe received"));

    endPoint->DoClose(kBleCloseFlag_AbortTransmission, BLE_ERROR_CENTRAL_UNSUBSCRIBED);
    return true;
}

bool BleLayer::HandleUnsubscribeComplete(BLE_CONNECTION_OBJECT connObj, const ChipBleUUID * svcId, const ChipBleUUID * charId)
{
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_SVC_ID, svcId), false, ChipLogError(Ble, "Unsubscribe complete on unknown svc"));
    VerifyOrReturnError(UUIDsMatch(&CHIP_BLE_CHAR_2_UUID, charId) || UUIDsMatch(&CHIP_BLE_CHAR_3_UUID, charId), false,
                        ChipLogError(Ble, "Unsubscribe complete on unknown char"));

    // Find end point already associated with BLE connection, if any.
    BLEEndPoint * endPoint = sBLEEndPointPool.Find(connObj);
    VerifyOrReturnError(endPoint != nullptr, false, ChipLogDetail(Ble, "No endpoint for unsubscribe complete"));

    endPoint->HandleUnsubscribeComplete();
    return true;
}

void BleLayer::HandleConnectionError(BLE_CONNECTION_OBJECT connObj, CHIP_ERROR err)
{
    // BLE connection has failed somehow, we must find and abort matching connection end point.
    BLEEndPoint * endPoint = sBLEEndPointPool.Find(connObj);
    VerifyOrReturn(endPoint != nullptr, ChipLogDetail(Ble, "No endpoint for connection error"));

    if (err == BLE_ERROR_GATT_UNSUBSCRIBE_FAILED && endPoint->IsUnsubscribePending())
    {
        // If end point was already closed and just waiting for unsubscribe to complete, free it. Call to Free()
        // stops unsubscribe timer.
        endPoint->Free();
    }
    else
    {
        endPoint->DoClose(kBleCloseFlag_AbortTransmission, err);
    }
}

BleTransportProtocolVersion BleLayer::GetHighestSupportedProtocolVersion(const BleTransportCapabilitiesRequestMessage & reqMsg)
{
    BleTransportProtocolVersion retVersion = kBleTransportProtocolVersion_None;

    uint8_t shift_width = 4;

    for (int i = 0; i < NUM_SUPPORTED_PROTOCOL_VERSIONS; i++)
    {
        shift_width ^= 4;

        uint8_t version = reqMsg.mSupportedProtocolVersions[(i / 2)];
        version         = static_cast<uint8_t>((version >> shift_width) & 0x0F); // Grab just the nibble we want.

        if ((version >= CHIP_BLE_TRANSPORT_PROTOCOL_MIN_SUPPORTED_VERSION) &&
            (version <= CHIP_BLE_TRANSPORT_PROTOCOL_MAX_SUPPORTED_VERSION) && (version > retVersion))
        {
            retVersion = static_cast<BleTransportProtocolVersion>(version);
        }
        else if (version == kBleTransportProtocolVersion_None) // Signifies end of supported versions list
        {
            break;
        }
    }

    return retVersion;
}

void BleLayer::OnConnectionComplete(void * appState, BLE_CONNECTION_OBJECT connObj)
{
    BleLayer * layer       = reinterpret_cast<BleLayer *>(appState);
    BLEEndPoint * endPoint = nullptr;
    CHIP_ERROR err         = CHIP_NO_ERROR;

    SuccessOrExit(err = layer->NewBleEndPoint(&endPoint, connObj, kBleRole_Central, true));
    layer->mBleTransport->OnBleConnectionComplete(endPoint);

exit:
    if (err != CHIP_NO_ERROR)
    {
        OnConnectionError(layer, err);
    }
}

void BleLayer::OnConnectionError(void * appState, CHIP_ERROR err)
{
    BleLayer * layer = reinterpret_cast<BleLayer *>(appState);
    layer->mBleTransport->OnBleConnectionError(err);
}

} /* namespace Ble */
} /* namespace chip */
