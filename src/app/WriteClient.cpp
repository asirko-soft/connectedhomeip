/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
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

/**
 *    @file
 *      This file defines the initiator side of a CHIP Write Interaction.
 *
 */

#include "lib/core/CHIPError.h"
#include <app/AppConfig.h>
#include <app/InteractionModelEngine.h>
#include <app/TimedRequest.h>
#include <app/WriteClient.h>

namespace chip {
namespace app {

void WriteClient::Close()
{
    MoveToState(State::AwaitingDestruction);

    if (mpCallback)
    {
        mpCallback->OnDone(this);
    }
}

CHIP_ERROR WriteClient::ProcessWriteResponseMessage(System::PacketBufferHandle && payload)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    System::PacketBufferTLVReader reader;
    TLV::TLVReader attributeStatusesReader;
    WriteResponseMessage::Parser writeResponse;
    AttributeStatusIBs::Parser attributeStatusesParser;

    reader.Init(std::move(payload));

    ReturnErrorOnFailure(writeResponse.Init(reader));

#if CHIP_CONFIG_IM_PRETTY_PRINT
    writeResponse.PrettyPrint();
#endif

    err = writeResponse.GetWriteResponses(&attributeStatusesParser);
    if (err == CHIP_END_OF_TLV)
    {
        return CHIP_NO_ERROR;
    }
    ReturnErrorOnFailure(err);

    attributeStatusesParser.GetReader(&attributeStatusesReader);

    while (CHIP_NO_ERROR == (err = attributeStatusesReader.Next()))
    {
        VerifyOrReturnError(TLV::AnonymousTag() == attributeStatusesReader.GetTag(), err = CHIP_ERROR_INVALID_TLV_TAG);

        AttributeStatusIB::Parser element;

        ReturnErrorOnFailure(element.Init(attributeStatusesReader));
        ReturnErrorOnFailure(ProcessAttributeStatusIB(element));
    }

    // if we have exhausted this container
    if (CHIP_END_OF_TLV == err)
    {
        err = CHIP_NO_ERROR;
    }
    ReturnErrorOnFailure(err);
    return writeResponse.ExitContainer();
}

CHIP_ERROR WriteClient::PrepareAttributeIB(const ConcreteDataAttributePath & aPath)
{
    AttributeDataIBs::Builder & writeRequests  = mWriteRequestBuilder.GetWriteRequests();
    AttributeDataIB::Builder & attributeDataIB = writeRequests.CreateAttributeDataIBBuilder();
    ReturnErrorOnFailure(writeRequests.GetError());
    if (aPath.mDataVersion.HasValue())
    {
        attributeDataIB.DataVersion(aPath.mDataVersion.Value());
        mHasDataVersion = true;
    }
    ReturnErrorOnFailure(attributeDataIB.GetError());
    AttributePathIB::Builder & path = attributeDataIB.CreatePath();

    // We are using kInvalidEndpointId just for group write requests. This is not the correct use of ConcreteDataAttributePath.
    // TODO: update AttributePathParams or ConcreteDataAttributePath for a class supports both nullable list index and missing
    // endpoint id.
    if (aPath.mEndpointId != kInvalidEndpointId)
    {
        path.Endpoint(aPath.mEndpointId);
    }
    path.Cluster(aPath.mClusterId).Attribute(aPath.mAttributeId);
    if (aPath.IsListItemOperation())
    {
        if (aPath.mListOp == ConcreteDataAttributePath::ListOperation::AppendItem)
        {
            path.ListIndex(DataModel::NullNullable);
        }
        else
        {
            // We do not support other list operations (i.e. update, delete etc) for now.
            return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
        }
    }
    ReturnErrorOnFailure(path.EndOfAttributePathIB());

    return CHIP_NO_ERROR;
}

CHIP_ERROR WriteClient::FinishAttributeIB()
{
    AttributeDataIB::Builder & attributeDataIB = mWriteRequestBuilder.GetWriteRequests().GetAttributeDataIBBuilder();
    ReturnErrorOnFailure(attributeDataIB.EndOfAttributeDataIB());
    MoveToState(State::AddAttribute);
    return CHIP_NO_ERROR;
}

TLV::TLVWriter * WriteClient::GetAttributeDataIBTLVWriter()
{
    return mWriteRequestBuilder.GetWriteRequests().GetAttributeDataIBBuilder().GetWriter();
}

CHIP_ERROR WriteClient::FinalizeMessage(bool aHasMoreChunks)
{
    System::PacketBufferHandle packet;
    VerifyOrReturnError(mState == State::AddAttribute, CHIP_ERROR_INCORRECT_STATE);

    TLV::TLVWriter * writer = mWriteRequestBuilder.GetWriter();
    VerifyOrReturnError(writer != nullptr, CHIP_ERROR_INCORRECT_STATE);
    ReturnErrorOnFailure(writer->UnreserveBuffer(kReservedSizeForTLVEncodingOverhead));

    ReturnErrorOnFailure(mWriteRequestBuilder.GetWriteRequests().EndOfAttributeDataIBs());

    ReturnErrorOnFailure(mWriteRequestBuilder.MoreChunkedMessages(aHasMoreChunks).EndOfWriteRequestMessage());
    ReturnErrorOnFailure(mMessageWriter.Finalize(&packet));
    mChunks.AddToEnd(std::move(packet));
    return CHIP_NO_ERROR;
}

CHIP_ERROR WriteClient::EnsureMessage()
{
    if (mState != State::AddAttribute)
    {
        return StartNewMessage();
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR WriteClient::StartNewMessage()
{
    uint16_t reservedSize = 0;

    if (mState == State::AddAttribute)
    {
        ReturnErrorOnFailure(FinalizeMessage(true));
    }

    // Do not allow timed request with chunks.
    VerifyOrReturnError(!(mTimedWriteTimeoutMs.HasValue() && !mChunks.IsNull()), CHIP_ERROR_NO_MEMORY);

    System::PacketBufferHandle packet = System::PacketBufferHandle::New(kMaxSecureSduLengthBytes);
    VerifyOrReturnError(!packet.IsNull(), CHIP_ERROR_NO_MEMORY);

    // Always limit the size of the packet to fit within kMaxSecureSduLengthBytes regardless of the available buffer capacity.
    if (packet->AvailableDataLength() > kMaxSecureSduLengthBytes)
    {
        reservedSize = static_cast<uint16_t>(packet->AvailableDataLength() - kMaxSecureSduLengthBytes);
    }

    // ... and we need to reserve some extra space for the MIC field.
    reservedSize = static_cast<uint16_t>(reservedSize + Crypto::CHIP_CRYPTO_AEAD_MIC_LENGTH_BYTES);

    // ... and the overhead for end of AttributeDataIBs (end of container), more chunks flag, end of WriteRequestMessage (another
    // end of container).
    reservedSize = static_cast<uint16_t>(reservedSize + kReservedSizeForTLVEncodingOverhead);

#if CONFIG_BUILD_FOR_HOST_UNIT_TEST
    // ... and for unit tests.
    reservedSize = static_cast<uint16_t>(reservedSize + mReservedSize);
#endif

    mMessageWriter.Init(std::move(packet));

    ReturnErrorOnFailure(mMessageWriter.ReserveBuffer(reservedSize));

    ReturnErrorOnFailure(mWriteRequestBuilder.Init(&mMessageWriter));
    mWriteRequestBuilder.SuppressResponse(mSuppressResponse);
    mWriteRequestBuilder.TimedRequest(mTimedWriteTimeoutMs.HasValue());
    ReturnErrorOnFailure(mWriteRequestBuilder.GetError());
    mWriteRequestBuilder.CreateWriteRequests();
    ReturnErrorOnFailure(mWriteRequestBuilder.GetError());

    TLV::TLVWriter * writer = mWriteRequestBuilder.GetWriter();
    VerifyOrReturnError(writer != nullptr, CHIP_ERROR_INCORRECT_STATE);

    return CHIP_NO_ERROR;
}

CHIP_ERROR WriteClient::TryPutSinglePreencodedAttributeWritePayload(const ConcreteDataAttributePath & attributePath,
                                                                    const TLV::TLVReader & data)
{
    TLV::TLVReader dataToWrite;
    dataToWrite.Init(data);

    TLV::TLVWriter * writer = nullptr;

    ReturnErrorOnFailure(PrepareAttributeIB(attributePath));
    VerifyOrReturnError((writer = GetAttributeDataIBTLVWriter()) != nullptr, CHIP_ERROR_INCORRECT_STATE);
    ReturnErrorOnFailure(writer->CopyElement(TLV::ContextTag(AttributeDataIB::Tag::kData), dataToWrite));
    ReturnErrorOnFailure(FinishAttributeIB());
    return CHIP_NO_ERROR;
}

CHIP_ERROR WriteClient::PutSinglePreencodedAttributeWritePayload(const chip::app::ConcreteDataAttributePath & attributePath,
                                                                 const TLV::TLVReader & data)
{
    TLV::TLVWriter backupWriter;

    mWriteRequestBuilder.GetWriteRequests().Checkpoint(backupWriter);

    // First attempt to write this attribute.
    CHIP_ERROR err = TryPutSinglePreencodedAttributeWritePayload(attributePath, data);
    if (err == CHIP_ERROR_NO_MEMORY || err == CHIP_ERROR_BUFFER_TOO_SMALL)
    {
        // If it failed with no memory, then we create a new chunk for it.
        mWriteRequestBuilder.GetWriteRequests().Rollback(backupWriter);
        ReturnErrorOnFailure(StartNewMessage());
        err = TryPutSinglePreencodedAttributeWritePayload(attributePath, data);
        // Since we have created a new chunk for this element, the encode is expected to succeed.
    }
    return err;
}

CHIP_ERROR WriteClient::PutPreencodedAttribute(const ConcreteDataAttributePath & attributePath, const TLV::TLVReader & data,
                                               TestListEncodingOverride testListEncodingOverride)
{
    ReturnErrorOnFailure(EnsureMessage());

    // ListIndex is missing and the data is an array -- we are writing a whole list.
    if (!attributePath.IsListOperation() && data.GetType() == TLV::TLVType::kTLVType_Array)
    {
        TLV::TLVReader dataReader;
        TLV::TLVReader valueReader;
        uint16_t encodedItemCount      = 0;
        ConcreteDataAttributePath path = attributePath;

        // By convention, and as tested against all cluster servers, clients have historically encoded an empty list as a
        // ReplaceAll, (i.e. the entire attribute contents are cleared before appending the new list’s items). However, this
        // behavior can be problematic, especially for the ACL attribute; sending an empty ReplaceAll list can cause clients to be
        // locked out. This is because the empty list first deletes all existing ACL entries, and if the new (malformed) ACL is
        // rejected, the server is left without valid (or with incomplete) ACLs.
        // SOLUTION: we treat ACL as an exception and avoid encoding an empty ReplaceAll list. Instead, we pack as many ACL entries
        // as possible into the ReplaceAll list, and send  any remaining entries in subsequent chunks are part of the AppendItem
        // list operation.
        // TODO (#38270): Generalize this behavior; send a non-empty ReplaceAll list for all clusters in a later Matter version and
        // enforce all clusters to support it in testing and in certification.
        bool encodeEmptyListAsReplaceAll = (path.mClusterId != Clusters::AccessControl::Id) ||
            (testListEncodingOverride == TestListEncodingOverride::kForceLegacyEncoding);

        if (encodeEmptyListAsReplaceAll)
        {
            ReturnErrorOnFailure(EncodeSingleAttributeDataIB(path, DataModel::List<uint8_t>()));
        }
        else
        {

            dataReader.Init(data);
            dataReader.OpenContainer(valueReader);
            bool chunkingNeeded = false;

            // Encode as many list-items as possible into a single AttributeDataIB, which will be included in a single
            // WriteRequestMessage chunk.
            ReturnErrorOnFailure(
                TryPutPreencodedAttributeWritePayloadIntoList(path, valueReader, chunkingNeeded, encodedItemCount));

            // If all list items fit perfectly into a single AttributeDataIB, there is no need for any `append-item` or chunking,
            // and we can exit early.
            VerifyOrReturnError(chunkingNeeded, CHIP_NO_ERROR);

            // Start a new WriteRequest chunk, as there are still remaining list items to encode. These remaining items will be
            // appended one by one, each into its own AttributeDataIB. Unlike the first chunk (which contains only one
            // AttributeDataIB), subsequent chunks may contain multiple AttributeDataIBs if space allows it.
            ReturnErrorOnFailure(StartNewMessage());
        }
        path.mListOp = ConcreteDataAttributePath::ListOperation::AppendItem;

        // We will restart iterating on ValueReader, only appending the items we need to append.
        dataReader.Init(data);
        dataReader.OpenContainer(valueReader);

        CHIP_ERROR err            = CHIP_NO_ERROR;
        uint16_t currentItemCount = 0;

        while ((err = valueReader.Next()) == CHIP_NO_ERROR)
        {
            currentItemCount++;

            if (currentItemCount <= encodedItemCount)
            {
                // Element already encoded via `TryPutPreencodedAttributeWritePayloadIntoList`
                continue;
            }

            ReturnErrorOnFailure(PutSinglePreencodedAttributeWritePayload(path, valueReader));
        }

        if (err == CHIP_END_OF_TLV)
        {
            err = CHIP_NO_ERROR;
        }
        return err;
    }

    // We are writing a non-list attribute, or we are writing a single element of a list.
    return PutSinglePreencodedAttributeWritePayload(attributePath, data);
}

CHIP_ERROR WriteClient::EnsureListStarted(const ConcreteDataAttributePath & attributePath)
{
    TLV::TLVWriter backupWriter;
    mWriteRequestBuilder.GetWriteRequests().Checkpoint(backupWriter);

    CHIP_ERROR err = TryToStartList(attributePath);
    if (err == CHIP_ERROR_NO_MEMORY || err == CHIP_ERROR_BUFFER_TOO_SMALL)
    {
        // If it failed with no memory, then we create a new chunk for it.
        mWriteRequestBuilder.GetWriteRequests().Rollback(backupWriter);
        ReturnErrorOnFailure(StartNewMessage());
        ReturnErrorOnFailure(TryToStartList(attributePath));
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR WriteClient::TryToStartList(const ConcreteDataAttributePath & attributePath)
{

    // TODO (#38414) : Move reservation/unreservtion of Buffer for TLV Writing to AttributeDataIB Builder instead of WriteClient
    ReturnErrorOnFailure(mMessageWriter.ReserveBuffer(kReservedSizeForEndOfListAttributeIB));

    ReturnErrorOnFailure(PrepareAttributeIB(attributePath));

    TLV::TLVWriter * writer = GetAttributeDataIBTLVWriter();
    VerifyOrReturnError(writer != nullptr, CHIP_ERROR_INCORRECT_STATE);

    TLV::TLVType outerType;
    ReturnErrorOnFailure(writer->StartContainer(TLV::ContextTag(AttributeDataIB::Tag::kData), TLV::kTLVType_Array, outerType));

    VerifyOrReturnError(outerType == kAttributeDataIBType, CHIP_ERROR_INCORRECT_STATE);

    return CHIP_NO_ERROR;
}

CHIP_ERROR WriteClient::EnsureListEnded()
{
    TLV::TLVWriter * writer = GetAttributeDataIBTLVWriter();
    VerifyOrReturnError(writer != nullptr, CHIP_ERROR_INCORRECT_STATE);

    // Undo the reservation made in EnsureListStarted() to free up space for the EndOfContainer TLV Elements
    // (for both the List and AttributeDataIB).
    ReturnErrorOnFailure(writer->UnreserveBuffer(kReservedSizeForEndOfListAttributeIB));
    ReturnErrorOnFailure(writer->EndContainer(kAttributeDataIBType));

    return FinishAttributeIB();
}

CHIP_ERROR
WriteClient::TryPutPreencodedAttributeWritePayloadIntoList(const ConcreteDataAttributePath & attributePath,
                                                           TLV::TLVReader & valueReader, bool & outChunkingNeeded,
                                                           ListIndex & outEncodedItemCount)
{

    ReturnErrorOnFailure(EnsureListStarted(attributePath));

    AttributeDataIB::Builder & attributeDataIB = mWriteRequestBuilder.GetWriteRequests().GetAttributeDataIBBuilder();
    TLV::TLVWriter backupWriter;
    CHIP_ERROR err      = CHIP_NO_ERROR;
    outEncodedItemCount = 0;

    while ((err = valueReader.Next()) == CHIP_NO_ERROR)
    {
        // Try to put all the list items into the list we just started, until we either run out of items
        // or run out of space.
        // Make sure that if we run out of space we don't leave a partially-encoded list item around.
        attributeDataIB.Checkpoint(backupWriter);
        err = attributeDataIB.GetWriter()->CopyElement(TLV::AnonymousTag(), valueReader);

        if (err == CHIP_ERROR_NO_MEMORY || err == CHIP_ERROR_BUFFER_TOO_SMALL)
        {
            // Rollback through the attributeDataIB, which also resets the Builder's error state.
            // This returns the object to the state it was in before attempting to copy the element.
            attributeDataIB.Rollback(backupWriter);
            outChunkingNeeded = true;
            err               = CHIP_NO_ERROR;
            break;
        }
        ReturnErrorOnFailure(err);
        outEncodedItemCount++;
    }
    VerifyOrReturnError(err == CHIP_END_OF_TLV || err == CHIP_NO_ERROR, err);

    return EnsureListEnded();
}

const char * WriteClient::GetStateStr() const
{
#if CHIP_DETAIL_LOGGING
    switch (mState)
    {
    case State::Initialized:
        return "Initialized";

    case State::AddAttribute:
        return "AddAttribute";

    case State::AwaitingTimedStatus:
        return "AwaitingTimedStatus";

    case State::AwaitingResponse:
        return "AwaitingResponse";

    case State::ResponseReceived:
        return "ResponseReceived";

    case State::AwaitingDestruction:
        return "AwaitingDestruction";
    }
#endif // CHIP_DETAIL_LOGGING
    return "N/A";
}

void WriteClient::MoveToState(const State aTargetState)
{
    mState = aTargetState;
    ChipLogDetail(DataManagement, "WriteClient moving to [%10.10s]", GetStateStr());
}

CHIP_ERROR WriteClient::SendWriteRequest(const SessionHandle & session, System::Clock::Timeout timeout)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit(mState == State::AddAttribute, err = CHIP_ERROR_INCORRECT_STATE);

    err = FinalizeMessage(false /* hasMoreChunks */);
    SuccessOrExit(err);

    {
        // Create a new exchange context.
        auto exchange = mpExchangeMgr->NewContext(session, this);
        VerifyOrExit(exchange != nullptr, err = CHIP_ERROR_NO_MEMORY);

        mExchangeCtx.Grab(exchange);
    }

    VerifyOrReturnError(!(mExchangeCtx->IsGroupExchangeContext() && mHasDataVersion), CHIP_ERROR_INVALID_MESSAGE_TYPE);

    if (timeout == System::Clock::kZero)
    {
        mExchangeCtx->UseSuggestedResponseTimeout(app::kExpectedIMProcessingTime);
    }
    else
    {
        mExchangeCtx->SetResponseTimeout(timeout);
    }

    if (mTimedWriteTimeoutMs.HasValue())
    {
        err = TimedRequest::Send(mExchangeCtx.Get(), mTimedWriteTimeoutMs.Value());
        SuccessOrExit(err);
        MoveToState(State::AwaitingTimedStatus);
    }
    else
    {
        err = SendWriteRequest();
        SuccessOrExit(err);
    }

exit:
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DataManagement, "Write client failed to SendWriteRequest: %" CHIP_ERROR_FORMAT, err.Format());
    }
    else
    {
        // TODO: Ideally this would happen async, but to make sure that we
        // handle this object dying (e.g. due to IM enging shutdown) while the
        // async bits are pending we'd need to malloc some state bit that we can
        // twiddle if we die.  For now just do the OnDone callback sync.
        if (session->IsGroupSession())
        {
            // Always shutdown on Group communication
            ChipLogDetail(DataManagement, "Closing on group Communication ");

            // Tell the application to release the object.
            // TODO: Consumers expect to hand off ownership of the WriteClient and wait for OnDone
            // after SendWriteRequest returns success.  Calling OnDone before returning is weird.
            // Need to refactor the code to avoid this.
            Close();
        }
    }

    return err;
}

CHIP_ERROR WriteClient::SendWriteRequest()
{
    using namespace Protocols::InteractionModel;
    using namespace Messaging;

    System::PacketBufferHandle data = mChunks.PopHead();

    bool isGroupWrite = mExchangeCtx->IsGroupExchangeContext();
    if (!mChunks.IsNull() && isGroupWrite)
    {
        // Reject this request if we have more than one chunk (mChunks is not null after PopHead()), and this is a group
        // exchange context.
        return CHIP_ERROR_INCORRECT_STATE;
    }

    // kExpectResponse is ignored by ExchangeContext in case of groupcast
    ReturnErrorOnFailure(mExchangeCtx->SendMessage(MsgType::WriteRequest, std::move(data), SendMessageFlags::kExpectResponse));

    MoveToState(State::AwaitingResponse);
    return CHIP_NO_ERROR;
}

CHIP_ERROR WriteClient::OnMessageReceived(Messaging::ExchangeContext * apExchangeContext, const PayloadHeader & aPayloadHeader,
                                          System::PacketBufferHandle && aPayload)
{
    using namespace Protocols::InteractionModel;

    if (mState == State::AwaitingResponse &&
        // We had sent the last chunk of data, and received all responses
        mChunks.IsNull())
    {
        MoveToState(State::ResponseReceived);
    }

    CHIP_ERROR err          = CHIP_NO_ERROR;
    bool sendStatusResponse = false;
    // Assert that the exchange context matches the client's current context.
    // This should never fail because even if SendWriteRequest is called
    // back-to-back, the second call will call Close() on the first exchange,
    // which clears the OnMessageReceived callback.
    VerifyOrExit(apExchangeContext == mExchangeCtx.Get(), err = CHIP_ERROR_INCORRECT_STATE);

    sendStatusResponse = true;

    if (mState == State::AwaitingTimedStatus)
    {
        if (aPayloadHeader.HasMessageType(MsgType::StatusResponse))
        {
            CHIP_ERROR statusError = CHIP_NO_ERROR;
            SuccessOrExit(err = StatusResponse::ProcessStatusResponse(std::move(aPayload), statusError));
            sendStatusResponse = false;
            SuccessOrExit(err = statusError);
            err = SendWriteRequest();
        }
        else
        {
            err = CHIP_ERROR_INVALID_MESSAGE_TYPE;
        }
        // Skip all other processing here (which is for the response to the
        // write request), no matter whether err is success or not.
        goto exit;
    }

    if (aPayloadHeader.HasMessageType(MsgType::WriteResponse))
    {
        err = ProcessWriteResponseMessage(std::move(aPayload));
        SuccessOrExit(err);
        sendStatusResponse = false;
        if (!mChunks.IsNull())
        {
            // Send the next chunk.
            SuccessOrExit(err = SendWriteRequest());
        }
    }
    else if (aPayloadHeader.HasMessageType(MsgType::StatusResponse))
    {
        CHIP_ERROR statusError = CHIP_NO_ERROR;
        SuccessOrExit(err = StatusResponse::ProcessStatusResponse(std::move(aPayload), statusError));
        SuccessOrExit(err = statusError);
        err = CHIP_ERROR_INVALID_MESSAGE_TYPE;
    }
    else
    {
        err = CHIP_ERROR_INVALID_MESSAGE_TYPE;
    }

exit:
    if (mpCallback != nullptr)
    {
        if (err != CHIP_NO_ERROR)
        {
            mpCallback->OnError(this, err);
        }
    }

    if (sendStatusResponse)
    {
        StatusResponse::Send(Status::InvalidAction, apExchangeContext, false /*aExpectResponse*/);
    }

    if (mState != State::AwaitingResponse)
    {
        Close();
    }
    // Else we got a response to a Timed Request and just sent the write.

    return err;
}

void WriteClient::OnResponseTimeout(Messaging::ExchangeContext * apExchangeContext)
{
    ChipLogError(DataManagement, "Time out! failed to receive write response from Exchange: " ChipLogFormatExchange,
                 ChipLogValueExchange(apExchangeContext));

    if (mpCallback != nullptr)
    {
        mpCallback->OnError(this, CHIP_ERROR_TIMEOUT);
    }
    Close();
}

CHIP_ERROR WriteClient::ProcessAttributeStatusIB(AttributeStatusIB::Parser & aAttributeStatusIB)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    AttributePathIB::Parser attributePathParser;
    StatusIB statusIB;
    StatusIB::Parser StatusIBParser;
    ConcreteDataAttributePath attributePath;

    err = aAttributeStatusIB.GetPath(&attributePathParser);
    SuccessOrExit(err);

    err = attributePathParser.GetConcreteAttributePath(attributePath);
    SuccessOrExit(err);

    err = aAttributeStatusIB.GetErrorStatus(&(StatusIBParser));
    if (CHIP_NO_ERROR == err)
    {
        err = StatusIBParser.DecodeStatusIB(statusIB);
        SuccessOrExit(err);
        if (mpCallback != nullptr)
        {
            mpCallback->OnResponse(this, attributePath, statusIB);
        }
    }

exit:
    return err;
}

} // namespace app
} // namespace chip
