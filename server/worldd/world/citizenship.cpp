// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "world/citizenship.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd::world {

namespace {

struct Db {
    sqlite3* db = nullptr;
    ~Db() {
        if (db) sqlite3_close(db);
    }
};

struct Stmt {
    sqlite3_stmt* s = nullptr;
    ~Stmt() {
        if (s) sqlite3_finalize(s);
    }
};

int weekday_of(const std::string& name) {
    static const char* const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    for (int i = 0; i < 7; ++i) {
        if (name == kDays[i]) return i;
    }
    return -1;
}

}  // namespace

int64_t CitizenshipRules::cooldown_ms(int level) const {
    const auto it = cooldown_secs.find(level);
    return it == cooldown_secs.end() ? 0 : std::max<int64_t>(0, it->second) * 1000;
}

int64_t CitizenshipRules::next_due_ms(int64_t from_ms) const {
    constexpr int64_t kDay = 86400000;
    const int64_t earliest = from_ms + static_cast<int64_t>(std::max(1, dues_weeks)) * 7 * kDay;
    int64_t day = earliest / kDay;   // days since 1970-01-01, a Thursday (weekday 4)
    if (earliest % kDay != 0) ++day;
    const int64_t weekday = (day + 4) % 7;
    const int64_t want = due_weekday >= 0 && due_weekday < 7 ? due_weekday : 4;
    day += (want - weekday + 7) % 7;
    return day * kDay;
}

std::optional<CitizenshipRules> CitizenshipRules::load(const std::string& path, std::string& error) {
    Db db;
    if (sqlite3_open_v2(path.c_str(), &db.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        error = "cannot open " + path + ": " + (db.db ? sqlite3_errmsg(db.db) : "out of memory");
        return std::nullopt;
    }
    CitizenshipRules r;
    {
        Stmt st;
        if (sqlite3_prepare_v2(db.db, "SELECT initial_due_cost FROM citizenship_generic ORDER BY guid LIMIT 1", -1, &st.s, nullptr) != SQLITE_OK) {
            error = path + " has no citizenship_generic table (" + sqlite3_errmsg(db.db) + ")";
            return std::nullopt;
        }
        if (sqlite3_step(st.s) == SQLITE_ROW) {
            const int cost = sqlite3_column_int(st.s, 0);
            if (cost > 0 && cost <= 100000) r.join_cost = cost;
        }
    }
    {
        Stmt st;
        if (sqlite3_prepare_v2(db.db, "SELECT level, cooldown_secs FROM citizenship_node ORDER BY level", -1, &st.s, nullptr) != SQLITE_OK) {
            error = path + " has no citizenship_node table (" + sqlite3_errmsg(db.db) + ")";
            return std::nullopt;
        }
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            const int level = sqlite3_column_int(st.s, 0);
            if (level >= 0 && level <= 6) r.cooldown_secs[level] = std::max<int64_t>(0, sqlite3_column_int64(st.s, 1));
        }
    }
    {
        Stmt st;
        if (sqlite3_prepare_v2(db.db, "SELECT due_day_of_week, weeks, late_fee, overdue_days FROM citizenship_dues_def ORDER BY guid LIMIT 1", -1, &st.s, nullptr) != SQLITE_OK) {
            error = path + " has no citizenship_dues_def table (" + sqlite3_errmsg(db.db) + ")";
            return std::nullopt;
        }
        if (sqlite3_step(st.s) == SQLITE_ROW) {
            const unsigned char* day = sqlite3_column_text(st.s, 0);
            if (day) {
                const int w = weekday_of(reinterpret_cast<const char*>(day));
                if (w >= 0) r.due_weekday = w;
            }
            const int weeks = sqlite3_column_int(st.s, 1);
            if (weeks > 0 && weeks <= 52) r.dues_weeks = weeks;
            const double fee = sqlite3_column_double(st.s, 2);
            if (fee >= 0 && fee <= 100000) r.late_fee = static_cast<int32_t>(std::lround(fee));
            const int days = sqlite3_column_int(st.s, 3);
            if (days >= 0 && days <= 365) r.overdue_days = days;
        }
    }
    {
        Stmt st;   // (a content.db without the column keeps the default)
        if (sqlite3_prepare_v2(db.db, "SELECT guid, citizenship_at_risk_secs FROM city_node", -1, &st.s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st.s) == SQLITE_ROW) r.at_risk_secs[sqlite3_column_int64(st.s, 0)] = sqlite3_column_int64(st.s, 1);
        }
    }
    if (r.cooldown_secs.empty()) {
        error = path + "'s citizenship_node lists no level: no node takes citizens";
        return std::nullopt;
    }
    return r;
}

}  // namespace aoc::worldd::world
