// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/schema/generated/ATrackedInteractableActor.h
//
//  From the client's class layout (AOCClient-Win64-Shipping.exe): FClassParams 0x14b604010, properties 0x14b603fb0 (InteractableGuid: FGuid, CPF_Net, RepNotify, offset 0x370).
//  AActor's commands and fields as AActor.h has them, then the class's own replicated properties, read from its FClassParams (CPF_Net) and
//  ordered by offset; their members from the usmap.  The class's RPCs are not listed (no open writes a field).
//
//  ATrackedInteractableActor: 22 RepLayout commands, 14 net fields.
//  Own: 1 replicated properties from handle 19.
//  Class chain:
//    UObject
//    AActor
//    ATrackedInteractableActor
// ============================================================================
#pragma once

#include "protocol/schema/generated/schema_types.h"

namespace aoc { namespace protocol { namespace schema { namespace gen {

namespace ATrackedInteractableActor_tables {

inline constexpr RepCommand kCommands[] = {
    {    1,    0, "AuthServerIDReplicated", "AActor", PropType::UInt32, 0,  32, nullptr, nullptr, 0, 0},
    {    2,    1, "bReplicateMovement", "AActor", PropType::Bool, 4,   1, nullptr, nullptr, 0, 0},
    {    3,    2, "bHidden", "AActor", PropType::Bool, 0,   1, nullptr, nullptr, 0, 0},
    {    4,    3, "bTearOff", "AActor", PropType::Bool, 0,   1, nullptr, nullptr, 0, 0},
    {    5,    4, "bCanBeDamaged", "AActor", PropType::Bool, 0,   1, nullptr, nullptr, 0, 0},
    {    6,    5, "bReplicates", "AActor", PropType::Bool, 4,   1, nullptr, nullptr, 0, 0},
    {    7,    6, "ReplicatedMovement", "AActor", PropType::NetSerializeStruct, 4,   0, "FRepMovement", nullptr, 0, 0},
    {    8,    7, "RemoteRole", "AActor", PropType::Enum, 0,   3, nullptr, "ENetRole", 0, 0},
    {    9,    8, "AttachmentReplication.AttachParent", "AActor", PropType::Object, 4, 128, nullptr, nullptr, 0, 0},
    {   10,    8, "AttachmentReplication.LocationOffset", "AActor", PropType::NetSerializeStruct, 4,   0, "FVector_NetQuantize100", nullptr, 0, 0},
    {   11,    8, "AttachmentReplication.RelativeScale3D", "AActor", PropType::NetSerializeStruct, 4,   0, "FVector_NetQuantize100", nullptr, 0, 0},
    {   12,    8, "AttachmentReplication.RotationOffset", "AActor", PropType::NetSerializeStruct, 4,   0, "FRotator", nullptr, 0, 0},
    {   13,    8, "AttachmentReplication.AttachSocket", "AActor", PropType::Name, 4,   0, nullptr, nullptr, 0, 0},
    {   14,    8, "AttachmentReplication.AttachComponent", "AActor", PropType::Object, 4, 128, nullptr, nullptr, 0, 0},
    {   15,    9, "Owner", "AActor", PropType::Object, 4, 128, nullptr, nullptr, 0, 0},
    {   16,   10, "Role", "AActor", PropType::Enum, 0,   3, nullptr, "ENetRole", 0, 0},
    {   17,   11, "NetDormancy", "AActor", PropType::Enum, 4,   3, nullptr, "ENetDormancy", 0, 0},
    {   18,   12, "Instigator", "AActor", PropType::Object, 4, 128, nullptr, nullptr, 0, 0},
    {   19,   13, "InteractableGuid.A", "ATrackedInteractableActor", PropType::Int32, 4,  32, nullptr, nullptr, 0, 0},
    {   20,   13, "InteractableGuid.B", "ATrackedInteractableActor", PropType::Int32, 4,  32, nullptr, nullptr, 0, 0},
    {   21,   13, "InteractableGuid.C", "ATrackedInteractableActor", PropType::Int32, 4,  32, nullptr, nullptr, 0, 0},
    {   22,   13, "InteractableGuid.D", "ATrackedInteractableActor", PropType::Int32, 4,  32, nullptr, nullptr, 0, 0},
};

inline constexpr NetField kFields[] = {
    {    0, "AuthServerIDReplicated", "AActor", FieldKind::Property, 0, 0x00000000, 1, kNoRpc},
    {    1, "bReplicateMovement", "AActor", FieldKind::Property, 4, 0x00000000, 2, kNoRpc},
    {    2, "bHidden", "AActor", FieldKind::Property, 0, 0x00000000, 3, kNoRpc},
    {    3, "bTearOff", "AActor", FieldKind::Property, 0, 0x00000000, 4, kNoRpc},
    {    4, "bCanBeDamaged", "AActor", FieldKind::Property, 0, 0x00000000, 5, kNoRpc},
    {    5, "bReplicates", "AActor", FieldKind::Property, 4, 0x00000000, 6, kNoRpc},
    {    6, "ReplicatedMovement", "AActor", FieldKind::Property, 4, 0x00000000, 7, kNoRpc},
    {    7, "RemoteRole", "AActor", FieldKind::Property, 0, 0x00000000, 8, kNoRpc},
    {    8, "AttachmentReplication", "AActor", FieldKind::Property, 4, 0x00000000, 9, kNoRpc},
    {    9, "Owner", "AActor", FieldKind::Property, 4, 0x00000000, 15, kNoRpc},
    {   10, "Role", "AActor", FieldKind::Property, 0, 0x00000000, 16, kNoRpc},
    {   11, "NetDormancy", "AActor", FieldKind::Property, 4, 0x00000000, 17, kNoRpc},
    {   12, "Instigator", "AActor", FieldKind::Property, 4, 0x00000000, 18, kNoRpc},
    {   13, "InteractableGuid", "ATrackedInteractableActor", FieldKind::Property, 4, 0x00000000, 19, kNoRpc},
};

inline constexpr uint16_t kCommandsByPath[] = {
    14, 9, 13, 10, 11, 12, 1, 18, 19, 20, 21, 22, 17, 15, 8, 7,
    16, 5, 3, 2, 6, 4,
};

inline constexpr uint16_t kFieldsByName[] = {
    8, 0, 12, 13, 11, 9, 7, 6, 10, 4, 2, 1, 5, 3,
};

}  // namespace ATrackedInteractableActor_tables

inline constexpr ClassSchema kATrackedInteractableActor{
    "ATrackedInteractableActor", "AActor", true, 19, 13,
    std::span<const RepCommand>{ATrackedInteractableActor_tables::kCommands},
    std::span<const RepCommand>{},
    std::span<const NetField>{ATrackedInteractableActor_tables::kFields},
    std::span<const RpcSchema>{},
    std::span<const uint16_t>{ATrackedInteractableActor_tables::kCommandsByPath},
    std::span<const uint16_t>{ATrackedInteractableActor_tables::kFieldsByName},
};

}}}}  // namespace aoc::protocol::schema::gen
