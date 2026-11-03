// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/marketplace_rpc.h
//
//  The player marketplace's calls of AAoCPlayerController, read and built over the schema (no recording of any of them exists; every layout is
//  the generated schema's, every meaning the client exe's, AOCClient-Win64-Shipping.exe, read-only):
//
//    the server's, to the client
//      ClientOpenMarketplaceUI(MarketplaceGuid: FGuid, NodeTaxDetails)              opens WBP_MarketplaceMenu (0x146684b30); the guid is the marketplace actor's,
//                                                                                    which every request below names again (the exe's check 0x145f008f0
//                                                                                    compares it with the one the controller keeps, then the distance)
//      ClientReceiveMarketplaceHeaders(NodeId, Headers[], HeaderVersion, SearchItemString, ItemRarity, bComplete)
//                                                                                    the storefronts.  UMarketplaceMenu's handler (0x1469dee30): a reply whose
//                                                                                    search and rarity equal the buy panel's current search is shown as that
//                                                                                    search's result; any other is merged into the menu's cache by PlayerGuid
//                                                                                    (a HeaderVersion above the cached one empties the cache first), and with
//                                                                                    bComplete the player's own header goes to the sell panel
//      ClientReceiveMarketplaceData(PlayerGuid, MarketplaceItemData[])              one storefront's listings (the Sell tab asks for the player's own)
//      ClientMarketplaceItemPurchaseResponse(MarketplaceItemGuid, Quantity, Response)  one per cart line; the menu finds the line in its cart by the guid
//      ClientMarketplaceItemRegistrationResponse(Item: FMarketplaceItemData, Response)
//      ClientMarketplaceItemRemovalResponse(MarketplaceItemGuid, Response)
//      ClientMarketplaceHeaderUpdateResponse(HeaderDescription, Response)
//    the client's, to the server
//      ServerRequestMarketplaceHeaders(MarketplaceActorGuid, HeaderVersion)         the menu opening (0x1469e1bc0)
//      ServerRequestMarketplaceData(MarketplaceActorGuid, PlayerGuid)               a storefront chosen (0x1469d0160) or the Sell tab (0x1469e2f60)
//      ServerRequestMarketplaceHeaderUpdate(MarketplaceActorGuid, HeaderDescription)  the storefront's description, cut to 50 characters by the client
//      ServerRequestMarketplaceItemPurchase(MarketplaceActorGuid, MarketplaceItemGuid, Quantity)  one call per cart line (0x1469de100)
//      ServerRequestMarketplaceItemRegistration(MarketplaceActorGuid, Item: FItemInstance, Quantity, PricePerUnit: FCurrencyContainer)
//      ServerRequestMarketplaceItemRemoval(MarketplaceActorGuid, MarketplaceItemGuid)
//      ServerSearchMarketplaceHeaders(MarketplaceActorGuid, SearchItemString, ItemRarity)  rarity 7 (EItemRarity::cCount) when none is chosen (0x1469e25e0)
//
//  EMarketplaceResponse and EMarketplaceItemStatus are the usmap's.
// ============================================================================
#pragma once

#include "protocol/repl/rpc.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

/// EMarketplaceResponse (5 bits on the wire).
enum class MarketplaceResponse : uint8_t {
    Success = 0, ErrorGeneric = 1, RequestExpired = 2, RequestAlreadyInProgress = 3, NodeNotFound = 4, ItemNotFound = 5, ItemUntradable = 6, ItemInUse = 7,
    InvalidQuantity = 8, InvalidPlayer = 9, InvalidPlayerLevel = 10, ItemIsLocked = 11, InventoryFull = 12, CurrencyNotFound = 13, InvalidDescriptionLen = 14,
    InvalidDescription = 15, InvalidSearchLen = 16, ServiceMaintenance = 17, FreeholdPurchaseFailure = 18, ItemDisabled = 19,
};

/// EMarketplaceItemStatus (3 bits).
enum class MarketplaceItemStatus : uint8_t { Listed = 0, Sold = 1, Removed = 2, Expired = 3, ExpiredClaimed = 4, ExpiredDeleted = 5 };

/// EItemRarity::cCount: what the client's search sends when no rarity is chosen.
inline constexpr uint8_t kMarketplaceAnyRarity = 7;
/// What the server sends as the ItemRarity of a reply that is no search: EItemRarity_MAX, a value the buy panel's search never holds, so the menu merges the reply into
/// its cache instead of showing it as a search result (modelled: no recording shows what retail sent).
inline constexpr uint8_t kMarketplaceNoSearchRarity = 8;

/// FMarketplaceHeader: one storefront (a player's listings at a node).
struct MarketplaceHeader {
    int64_t     node = 0;
    GuidValue   player;
    std::string player_name;
    std::string description;
    double      issue_date = 0;
    bool        city_node_header = false;
    bool operator==(const MarketplaceHeader&) const = default;
};

/// FMarketplaceItemData: one listing.
struct MarketplaceItemData {
    int64_t               node = 0;
    GuidValue             player;
    std::string           player_name;
    GuidValue             item_guid;           // the listing's MarketplaceItemGuid
    ItemInstance          item;
    std::string           item_name;
    CurrencyContainer     price_per_unit;
    double                issue_date = 0;
    double                expiration_date = 0;
    MarketplaceItemStatus status = MarketplaceItemStatus::Listed;
    bool operator==(const MarketplaceItemData&) const = default;
};

// ---- the client's requests --------------------------------------------------------------------------------------------------------------------------------

struct MarketHeadersRequest {
    GuidValue market;
    uint64_t  version = 0;
    bool operator==(const MarketHeadersRequest&) const = default;
};
struct MarketDataRequest {
    GuidValue market;
    GuidValue player;
    bool operator==(const MarketDataRequest&) const = default;
};
struct MarketHeaderUpdateRequest {
    GuidValue   market;
    std::string description;
    bool operator==(const MarketHeaderUpdateRequest&) const = default;
};
struct MarketPurchaseRequest {
    GuidValue market;
    GuidValue item_guid;
    int32_t   quantity = 0;
    bool operator==(const MarketPurchaseRequest&) const = default;
};
struct MarketRegistrationRequest {
    GuidValue         market;
    ItemInstance      item;
    int32_t           quantity = 0;
    CurrencyContainer price_per_unit;
    bool operator==(const MarketRegistrationRequest&) const = default;
};
struct MarketRemovalRequest {
    GuidValue market;
    GuidValue item_guid;
    bool operator==(const MarketRemovalRequest&) const = default;
};
struct MarketSearchRequest {
    GuidValue   market;
    std::string search;
    uint8_t     rarity = kMarketplaceAnyRarity;
    bool operator==(const MarketSearchRequest&) const = default;
};

/// Each from a decoded call of its RPC; nullopt for another RPC, an undecoded call or a parameter of another kind.  A parameter left at its default is
/// not on the wire and reads as its default (a zero guid, an empty string, 0).
std::optional<MarketHeadersRequest>      parse_market_headers_request(const RpcCall& call);
std::optional<MarketDataRequest>         parse_market_data_request(const RpcCall& call);
std::optional<MarketHeaderUpdateRequest> parse_market_header_update_request(const RpcCall& call);
std::optional<MarketPurchaseRequest>     parse_market_purchase_request(const RpcCall& call);
std::optional<MarketRegistrationRequest> parse_market_registration_request(const RpcCall& call);
std::optional<MarketRemovalRequest>      parse_market_removal_request(const RpcCall& call);
std::optional<MarketSearchRequest>       parse_market_search_request(const RpcCall& call);

/// The same requests as parameters (the tests write a client's call with them).
std::optional<RpcParams> market_headers_request_params(const schema::RpcSchema& rpc, const MarketHeadersRequest& r);
std::optional<RpcParams> market_data_request_params(const schema::RpcSchema& rpc, const MarketDataRequest& r);
std::optional<RpcParams> market_header_update_request_params(const schema::RpcSchema& rpc, const MarketHeaderUpdateRequest& r);
std::optional<RpcParams> market_purchase_request_params(const schema::RpcSchema& rpc, const MarketPurchaseRequest& r);
std::optional<RpcParams> market_registration_request_params(const schema::RpcSchema& rpc, const MarketRegistrationRequest& r);
std::optional<RpcParams> market_removal_request_params(const schema::RpcSchema& rpc, const MarketRemovalRequest& r);
std::optional<RpcParams> market_search_request_params(const schema::RpcSchema& rpc, const MarketSearchRequest& r);

// ---- the server's answers ---------------------------------------------------------------------------------------------------------------------------------

/// ClientOpenMarketplaceUI.
std::optional<RpcParams> open_marketplace_params(const schema::RpcSchema& rpc, const GuidValue& market, const NodeTaxDetails& taxes);

/// ClientReceiveMarketplaceHeaders.
struct MarketHeaders {
    int64_t                        node = 0;
    std::vector<MarketplaceHeader> headers;
    uint64_t                       version = 0;
    std::string                    search;
    uint8_t                        rarity = kMarketplaceNoSearchRarity;
    bool                           complete = true;
    bool operator==(const MarketHeaders&) const = default;
};
std::optional<RpcParams>     market_headers_params(const schema::RpcSchema& rpc, const MarketHeaders& h);
std::optional<MarketHeaders> parse_market_headers(const RpcCall& call);

/// ClientReceiveMarketplaceData.
struct MarketData {
    GuidValue                        player;
    std::vector<MarketplaceItemData> items;
    bool operator==(const MarketData&) const = default;
};
std::optional<RpcParams>  market_data_params(const schema::RpcSchema& rpc, const MarketData& d);
std::optional<MarketData> parse_market_data(const RpcCall& call);

/// ClientMarketplaceItemPurchaseResponse, ClientMarketplaceItemRegistrationResponse, ClientMarketplaceItemRemovalResponse, ClientMarketplaceHeaderUpdateResponse.
std::optional<RpcParams> market_purchase_response_params(const schema::RpcSchema& rpc, const GuidValue& item_guid, int32_t quantity, MarketplaceResponse response);
std::optional<RpcParams> market_registration_response_params(const schema::RpcSchema& rpc, const MarketplaceItemData& item, MarketplaceResponse response);
std::optional<RpcParams> market_removal_response_params(const schema::RpcSchema& rpc, const GuidValue& item_guid, MarketplaceResponse response);
std::optional<RpcParams> market_header_update_response_params(const schema::RpcSchema& rpc, const std::string& description, MarketplaceResponse response);

/// A response read back (the tests): the guid (zero for the header update), the quantity (purchase only), the description (header update only) and the response.
struct MarketResponse {
    GuidValue           item_guid;
    int32_t             quantity = 0;
    std::string         description;
    MarketplaceItemData item;   // registration only
    MarketplaceResponse response = MarketplaceResponse::Success;
    bool operator==(const MarketResponse&) const = default;
};
std::optional<MarketResponse> parse_market_response(const RpcCall& call);

}}}  // namespace aoc::protocol::repl
