// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_LLCPP_DECODED_MESSAGE_H_
#define LIB_FIDL_LLCPP_DECODED_MESSAGE_H_

#include <lib/fidl/coding.h>
#include <lib/fidl/llcpp/encoded_message.h>
#include <lib/fidl/llcpp/traits.h>
#include <type_traits>
#include <zircon/fidl.h>

namespace fidl {

template<typename FidlType>
struct DecodeResult;

template <typename FidlType>
DecodeResult<FidlType> Decode(EncodedMessage<FidlType> msg);

template<typename FidlType>
struct EncodeResult;

template <typename FidlType>
class DecodedMessage;

template <typename FidlType>
EncodeResult<FidlType> Encode(DecodedMessage<FidlType> msg);

// `DecodedMessage` manages a linearized FIDL message in decoded form.
// It takes care of releasing all handles which were not consumed
// (std::moved from the decoded FIDL struct) when it goes out of scope.
template <typename FidlType>
class DecodedMessage final {
    static_assert(IsFidlType<FidlType>::value, "Only FIDL types allowed here");
    static_assert(FidlType::PrimarySize > 0, "Positive message size");

public:
    // Instantiates an empty message.
    // To populate this message, decode from an EncodedMessage object.
    DecodedMessage() = default;

    // Instantiates a DecodedMessage which points to a buffer region with caller-managed memory.
    // The buffer region is assumed to contain a linearized FIDL message with valid pointers.
    // This does not take ownership of that buffer region.
    // But it does take ownership of the handles within the buffer.
    explicit DecodedMessage(BytePart bytes) : bytes_(std::move(bytes)) {
        ZX_DEBUG_ASSERT(bytes_.actual() >= FidlType::PrimarySize);
    }

    DecodedMessage(DecodedMessage&& other) = default;

    DecodedMessage& operator=(DecodedMessage&& other) = default;

    DecodedMessage(const DecodedMessage& other) = delete;

    DecodedMessage& operator=(const DecodedMessage& other) = delete;

    ~DecodedMessage() {
        CloseHandles();
    }

    // Keeps track of a new buffer region with caller-managed memory.
    // The buffer region is assumed to contain a linearized FIDL message with valid pointers.
    // This does not take ownership of that buffer region.
    // But it does take ownership of the handles within the buffer.
    void Reset(BytePart bytes) {
        CloseHandles();
        bytes_ = std::move(bytes);
    }

    // Accesses the FIDL message by reinterpreting the buffer pointer.
    // Returns nullptr if there is no message.
    FidlType* message() const {
        return reinterpret_cast<FidlType*>(bytes_.data());
    }

    // Returns true iff the DecodedMessage has a valid message, i.e. non-NULL buffer pointer.
    bool is_valid() const {
        return bytes_.data() != nullptr;
    }

private:
    friend DecodeResult<FidlType> Decode<FidlType>(EncodedMessage<FidlType> msg);

    friend EncodeResult<FidlType> Encode<FidlType>(DecodedMessage<FidlType> msg);

    // Use the FIDL encoding tables for |FidlType| to walk the message and
    // destroy the handles it contains.
    void CloseHandles() {
        // Using the coding table to single out boring types instead of checking
        // |FidlType::MaxNumHandle|, to avoid the pathological case where a FIDL message has a
        // vector of handles with max count limited at 0, but the user attaches some handles anyway.
        if (!NeedsEncodeDecode<FidlType>::value) {
            return;
        }
#ifdef __Fuchsia__
        if (bytes_.data()) {
            fidl_close_handles(FidlType::Type, bytes_.data(), nullptr);
        }
#endif
    }

    // The contents of the decoded message.
    BytePart bytes_;
};

}  // namespace fidl

#endif // LIB_FIDL_LLCPP_DECODED_MESSAGE_H_
