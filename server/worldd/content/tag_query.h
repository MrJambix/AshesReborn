// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  content/tag_query.h  (the vendors' sell filters, contentc version 19; the quests' Kill objectives by tag query, contentc version 43)
//
//  A gameplay tag query as content.db holds one ({"tok": [the token stream], "tags": [the dictionary]}: vendor_modifier.filter, vendor_sell_filter.filter, and since
//  contentc version 43 a quest objective's TagQuery and NPCTagQuery) and how it is evaluated against a set of tags.
//
//  The token stream: a version, a non-empty flag, then the tree -- an expression kind and a count; kinds 1 to 3 hold any, all or none of the tags that follow (indexes
//  into the dictionary), kinds 4 to 6 any, all or none of the sub-expressions that follow.  The tags a query is evaluated against are lower case and hold every tag
//  with all its parents ("character.beast.wolf" is also "character.beast" and "character"): a query tag matches itself and its descendants, as the game's do.
//  A query that is not there, or whose stream does not read, matches nothing.
// ============================================================================
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::content {

/// A gameplay tag query as the data holds it: the token stream and the tag dictionary (lower case); `present` is false for none.
struct TagQuery {
    bool                     present = false;
    std::vector<int64_t>     tok;
    std::vector<std::string> tags;
};

/// A set of tags made for the queries: lower case, every tag with all its parents, sorted, each once.
using TagSet = std::vector<std::string>;

/// The TagSet of `tags` (any case; empty ones are left out).
inline TagSet tag_set_with_parents(const std::vector<std::string>& tags) {
    std::set<std::string> out;
    for (const std::string& t : tags) {
        std::string lower;
        lower.reserve(t.size());
        for (const char c : t) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (lower.empty()) continue;
        for (size_t dot = lower.find('.'); dot != std::string::npos; dot = lower.find('.', dot + 1)) {
            if (dot > 0) out.insert(lower.substr(0, dot));
        }
        out.insert(std::move(lower));
    }
    return TagSet(out.begin(), out.end());
}

namespace detail {

template <typename Has>
struct TagEval {
    const TagQuery& q;
    const Has&      have;   // have(lower-case tag): whether the set holds it
    size_t          pos = 2;

    bool has(int64_t i) const { return i >= 0 && static_cast<size_t>(i) < q.tags.size() && have(q.tags[static_cast<size_t>(i)]); }

    bool expr(bool& good, int depth = 0) {
        if (depth > 64 || pos + 1 >= q.tok.size()) {
            good = false;
            return false;
        }
        const int64_t kind = q.tok[pos], n = q.tok[pos + 1];
        pos += 2;
        if (kind >= 1 && kind <= 3) {
            if (n < 0 || pos + static_cast<size_t>(n) > q.tok.size()) {
                good = false;
                return false;
            }
            bool any = false, all = true;
            for (int64_t i = 0; i < n; ++i) {
                if (has(q.tok[pos + static_cast<size_t>(i)])) any = true;
                else all = false;
            }
            pos += static_cast<size_t>(n);
            return kind == 1 ? any : kind == 2 ? all : !any;
        }
        if (kind >= 4 && kind <= 6) {
            bool any = false, all = true;
            for (int64_t i = 0; i < n; ++i) {
                const bool r = expr(good, depth + 1);
                if (!good) return false;
                if (r) any = true;
                else all = false;
            }
            return kind == 4 ? any : kind == 5 ? all : !any;
        }
        good = false;
        return false;
    }
};

template <typename Has>
bool evaluate(const TagQuery& q, const Has& have) {
    if (!q.present) return false;
    TagEval<Has> e{q, have};
    bool         good = true;
    const bool   r = e.expr(good);
    return r && good;
}

}  // namespace detail

/// Whether a tag query holds for a set of lower-case tags (each tag with its parents).  A query that is not there, or whose stream does not read, matches nothing.
inline bool tag_query_matches(const TagQuery& q, const std::set<std::string>& have) {
    return detail::evaluate(q, [&](const std::string& t) { return have.count(t) != 0; });
}

/// ... for a TagSet (tag_set_with_parents).
inline bool tag_query_matches(const TagQuery& q, const TagSet& have) {
    return detail::evaluate(q, [&](const std::string& t) { return std::binary_search(have.begin(), have.end(), t); });
}

}  // namespace aoc::worldd::content
