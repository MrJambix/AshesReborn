// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/market.inc: the members of PlayerSession for the player marketplace (session/player_market.cpp, market/market_book.h)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// Whether an NPC is a marketplace (its interaction opens EActionResponse::OpenMarketplaceWindow).
    bool is_marketplace(const world::WorldActor& a) const;
    /// A use of a marketplace NPC: proceeds owed are paid, then ClientOpenMarketplaceUI with the node's taxes.
    void open_marketplace(Connection& conn, const world::WorldActor& npc, Clock::time_point now);
    /// The open marketplace for a request naming `market`: nullptr (with `why`) when none is open, the guid is another, the NPC is gone or out of reach.
    const world::WorldActor* open_market(const repl::GuidValue& market, std::string& why);
    void on_market_headers(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_market_data(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_market_header_update(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_market_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_market_registration(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_market_removal(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_market_search(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// The listings of `seller_id` at the open market as ClientReceiveMarketplaceData (the Sell tab's when it is the player's own).
    void send_market_data(Connection& conn, const std::string& seller_id, const repl::GuidValue& seller, bool own);
    /// The proceeds the player is owed at every market, paid into the purse (what fits below the cap): the rows go with the next save.
    void collect_market_proceeds(Connection& conn);
    /// What the book changed by this player's act, saved now with the inventory and the purse, in the same transaction.
    void save_market(std::vector<svc::MarketRow> rows, const char* why);
    /// The client's view of a listing.
    repl::MarketplaceItemData market_item(const market::Listing& l) const;
    double market_clock() const;
    std::string market_seller_id() const { return character_id_; }
    repl::GuidValue market_player() const;
    std::shared_ptr<market::Book> market_book_;
    uint64_t                      market_actor_ = 0;      // the marketplace NPC whose window is open (0: none)
    repl::GuidValue               market_guid_{};         // the MarketplaceGuid the window was opened with
    int64_t                       market_node_ = 0;       // the node whose market it is
    std::vector<svc::MarketRow>   market_rows_;           // rows waiting for the next inventory save (hand_over_inventory takes them)
    RequestBudget                 market_budget_{kVendorBurst, kVendorInterval};
    size_t                        market_logged_ = 0;
    uint32_t                      market_opens_ = 0, market_listed_ = 0, market_bought_ = 0, market_removed_ = 0, market_refused_ = 0;
public:
    uint32_t market_opens() const { return market_opens_; }
    uint32_t market_listed() const { return market_listed_; }
    uint32_t market_bought() const { return market_bought_; }
    uint32_t market_removed() const { return market_removed_; }
    uint32_t market_refused() const { return market_refused_; }
    /// A test: the marketplace this session would open with `npc` (the answer is queued as a use would).
    void open_marketplace_for_test(Connection& conn, const world::WorldActor& npc, Clock::time_point now) { open_marketplace(conn, npc, now); }
private:
