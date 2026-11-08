// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/citizenship.inc: the members of PlayerSession for node citizenship (session/player_citizenship.cpp, world/citizenship.h)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// What the Node Administrator offered last: the join dialog or the renounce warning, for that NPC's node.  The application must name that node.
    struct CitizenshipOffer {
        uint64_t          npc = 0;      // the NPC (actor id)
        int64_t           node = 0;     // its node
        int64_t           vendor = 0;   // the shop of the tax receipts the dialog's purchase button opens
        int32_t           cost = 0;     // the Tax Receipts the dialog asked for
        bool              join = true;  // the join dialog (false: the renounce warning)
        Clock::time_point at{};
    };
    /// How long the offer stands (modelled: the client keeps its dialog open as long as the player likes; the server takes an answer within this).
    static constexpr std::chrono::minutes kCitizenshipOfferFor{10};
    /// The Node Administrator's line (dialogue AddNodeCitizenship or RemoveNodeCitizenship): ClientShowAddNodeCitizenshipDialog or ClientShowCitizenshipRenouncementWarning,
    /// or ClientCitizenshipApplicationRejection when the player cannot join that node.
    void open_citizenship_dialog(Connection& conn, const world::WorldActor& npc, bool join, int64_t vendor, Clock::time_point now);
    /// The NPC of the standing join offer whose shop is `vendor` and who is within reach (the dialog's purchase button, ServerOpenVendor, after the conversation ended); null for none.
    const world::WorldActor* citizenship_offer_npc(int64_t vendor, Clock::time_point now) const;
    void on_citizenship_application(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_citizenship_renounce(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_citizenship_dues(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// ClientPopulateCitizenshipData: the window's data for the account's citizenship now.
    void send_citizenship_window(Connection& conn);
    repl::CitizenshipWindowData citizenship_window(int64_t now_ms) const;
    /// The account's citizenship becomes `c`: the world's copy, OwnerInfo (CitizenNodeGuid, CharactersNodeData) and a progress save.
    void set_citizenship(Connection& conn, const svc::Citizenship& c, const char* why);
    /// One reliable call of the controller with `params` set by `fill` (false: it does not build, nothing goes out).
    void send_citizenship_call(Connection& conn, const char* rpc, std::function<bool(const protocol::schema::RpcSchema&, repl::RpcParams&)> fill);
    /// Takes `cost` Tax Receipts (the TaxVoucher items); the events go to the client and the bag is saved.  False with `why`, changing nothing, when the bags hold fewer.
    bool pay_tax_receipts(Connection& conn, int32_t cost, std::string& why);
    /// Every few seconds: the account's citizenship as another session of it changed it (adopted, not saved again), and a citizenship whose dues stayed unpaid too long lapses.
    void run_citizenship(Connection& conn);
    // ---- stage 2: mayors
    /// This character as the elections know a citizen, a candidate or a voter (the account, the character, its name).
    world::ElectionPerson election_person() const;
    /// The account's primary character as the elections' citizen.
    world::ElectionPerson citizen_person() const;
    /// The Sheriff's "stand for mayor" line (dialogue AddNodeMayorCandidate) and ServerAddNodeMayorCandidate: the character stands in its citizenship's node.
    void stand_for_mayor(Connection& conn, const char* by);
    void on_mayor_candidate(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_mayor_ranked_vote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_mayor_named_vote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_mayor_message(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_mayor_citizens(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// The elections' news for this session: OwnerInfo.MayorNode, the results of its node's election (ClientFinishMayorVote).
    void run_elections(Connection& conn, int64_t now_ms);
    /// `.election`: the election of the node the player stands in; `.election next` (a game master with GM mode on) moves it to its next phase now.
    std::string on_election_command(Connection& conn, const std::string& args);
    /// The node at the NPC's place: its guid and level (0 and -1 for none).
    std::pair<int64_t, int> citizenship_node_at(double x, double y) const;
    svc::Citizenship                citizenship_;
    bool                            citizenship_changed_ = false;   // this session changed it: every progress save carries it
    std::optional<CitizenshipOffer> citizenship_offer_;
    RequestBudget                   citizenship_budget_{kVendorBurst, kVendorInterval};
    size_t                          citizenship_log_ = 0;
    int64_t                         citizenship_checked_ms_ = 0;   // run_citizenship's last look
    uint64_t                        election_seen_version_ = 0;    // the elections' version run_elections last looked at
    int                             election_seen_state_ = -1;     // the state of the citizenship node's election then (-1: none)
    int64_t                         mayor_node_ = 0;               // OwnerInfo.MayorNode as last set
    uint32_t                        mayor_candidacies_ = 0, mayor_votes_ = 0, mayor_results_seen_ = 0;
    uint32_t citizenship_lapses_ = 0, citizenship_adopted_ = 0;
    uint32_t citizenship_joins_ = 0, citizenship_renounces_ = 0, citizenship_dues_paid_ = 0, citizenship_refused_ = 0, citizenship_windows_ = 0, citizenship_dialogs_ = 0;
public:
    const svc::Citizenship& citizenship() const { return citizenship_; }
    uint32_t citizenship_joins() const { return citizenship_joins_; }
    uint32_t citizenship_renounces() const { return citizenship_renounces_; }
    uint32_t citizenship_dues_paid() const { return citizenship_dues_paid_; }
    uint32_t citizenship_refused() const { return citizenship_refused_; }
    uint32_t citizenship_windows() const { return citizenship_windows_; }
    uint32_t citizenship_dialogs() const { return citizenship_dialogs_; }
    uint32_t citizenship_lapses() const { return citizenship_lapses_; }
    uint32_t citizenship_adopted() const { return citizenship_adopted_; }
    uint32_t mayor_candidacies() const { return mayor_candidacies_; }
    uint32_t mayor_votes() const { return mayor_votes_; }
    uint32_t mayor_results_seen() const { return mayor_results_seen_; }
    int64_t  mayor_node() const { return mayor_node_; }
    std::string election_command_for_test(Connection& conn, const std::string& args) { return on_election_command(conn, args); }
    /// A test: run_citizenship now (not waiting for its few seconds).
    void run_citizenship_for_test(Connection& conn) { citizenship_checked_ms_ = 0; run_citizenship(conn); }
    /// The node of the standing offer (0: none).
    int64_t citizenship_offer_node() const { return citizenship_offer_ ? citizenship_offer_->node : 0; }
    /// A test: the Node Administrator's line for `npc`, as the conversation runs it.
    void open_citizenship_dialog_for_test(Connection& conn, const world::WorldActor& npc, bool join, int64_t vendor, Clock::time_point now) {
        open_citizenship_dialog(conn, npc, join, vendor, now);
    }
    /// A test: the account's citizenship as the join read it.
    void set_citizenship_for_test(const svc::Citizenship& c) { citizenship_ = c; }
private:
