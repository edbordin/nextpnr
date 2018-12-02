/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  David Shah <david@symbioticeda.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

/*
 * Timing-optimised detailed placement algorithm using BFS of the neighbour graph created from cells
 * on a critical path
 *
 * Based on "An Effective Timing-Driven Detailed Placement Algorithm for FPGAs"
 * https://www.cerc.utexas.edu/utda/publications/C205.pdf
 *
 * Modifications made to deal with the smaller Bels that nextpnr uses instead of swapping whole tiles,
 * and deal with the fact that not every cell on the crit path may be swappable.
 */

#include "timing.h"
#include "timing_opt.h"
#include "nextpnr.h"
#include "util.h"
#include <boost/range/adaptor/reversed.hpp>
#include <queue>

namespace std {

    template <> struct hash<std::pair<NEXTPNR_NAMESPACE_PREFIX IdString, NEXTPNR_NAMESPACE_PREFIX IdString>>
    {
        std::size_t operator()(const std::pair<NEXTPNR_NAMESPACE_PREFIX IdString, NEXTPNR_NAMESPACE_PREFIX IdString> &idp) const noexcept
        {
            std::size_t seed = 0;
            boost::hash_combine(seed, hash<NEXTPNR_NAMESPACE_PREFIX IdString>()(idp.first));
            boost::hash_combine(seed, hash<NEXTPNR_NAMESPACE_PREFIX IdString>()(idp.second));
            return seed;
        }
    };

    template <> struct hash<std::pair<int, NEXTPNR_NAMESPACE_PREFIX BelId>>
    {
        std::size_t operator()(const std::pair<int, NEXTPNR_NAMESPACE_PREFIX BelId> &idp) const noexcept
        {
            std::size_t seed = 0;
            boost::hash_combine(seed, hash<int>()(idp.first));
            boost::hash_combine(seed, hash<NEXTPNR_NAMESPACE_PREFIX BelId>()(idp.second));
            return seed;
        }
    };

    template <> struct hash<std::pair<NEXTPNR_NAMESPACE_PREFIX IdString, NEXTPNR_NAMESPACE_PREFIX BelId>>
    {
        std::size_t operator()(const std::pair<NEXTPNR_NAMESPACE_PREFIX IdString, NEXTPNR_NAMESPACE_PREFIX BelId> &idp) const noexcept
        {
            std::size_t seed = 0;
            boost::hash_combine(seed, hash<NEXTPNR_NAMESPACE_PREFIX IdString>()(idp.first));
            boost::hash_combine(seed, hash<NEXTPNR_NAMESPACE_PREFIX BelId>()(idp.second));
            return seed;
        }
    };
}

NEXTPNR_NAMESPACE_BEGIN

class TimingOptimiser
{
  public:
    TimingOptimiser(Context *ctx, TimingOptCfg cfg) : ctx(ctx), cfg(cfg) {};
    bool optimise() {
        log_info("Running timing-driven placement optimisation...\n");
#if 1
        timing_analysis(ctx, false, true, ctx->debug, false);
#endif
        for (int i = 0; i < 20; i++) {
            log_info("   Iteration %d...\n", i);
            get_criticalities(ctx, &net_crit);
            setup_delay_limits();
            auto crit_paths = find_crit_paths(0.98, 1000);
            for (auto &path : crit_paths)
                optimise_path(path);
#if 1
            timing_analysis(ctx, false, true, ctx->debug, false);
#endif
        }
        return true;
    }

  private:
    // Ratio of available to already-candidates to begin borrowing
    const float borrow_thresh = 0.2;

    void setup_delay_limits() {
        max_net_delay.clear();
        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            for (auto usr : ni->users) {
                max_net_delay[std::make_pair(usr.cell->name, usr.port)]
                        = std::numeric_limits<delay_t>::max();
            }
            if (!net_crit.count(net.first))
                continue;
            auto &nc = net_crit.at(net.first);
            if (nc.slack.empty())
                continue;
            for (size_t i = 0; i < ni->users.size(); i++) {
                auto &usr = ni->users.at(i);
                delay_t net_delay = ctx->getNetinfoRouteDelay(ni, usr);
                if (nc.max_path_length != 0) {
                    max_net_delay[std::make_pair(usr.cell->name, usr.port)]
                            = net_delay + ((nc.slack.at(i) - nc.cd_worst_slack) / nc.max_path_length);
                }
            }
        }
    }

    bool check_cell_delay_limits(CellInfo *cell) {
        for (const auto &port : cell->ports) {
            int nc;
            if (ctx->getPortTimingClass(cell, port.first, nc) == TMG_IGNORE)
                continue;
            NetInfo *net = port.second.net;
            if (net == nullptr)
                continue;
            if (port.second.type == PORT_IN) {
                if (net->driver.cell == nullptr || net->driver.cell->bel == BelId())
                    continue;
                BelId srcBel = net->driver.cell->bel;
                if (ctx->estimateDelay(ctx->getBelPinWire(srcBel, net->driver.port),
                        ctx->getBelPinWire(cell->bel, port.first)) > max_net_delay.at(std::make_pair(cell->name, port.first)))
                    return false;
            } else if (port.second.type == PORT_OUT) {
                for (auto user : net->users) {
                    // This could get expensive for high-fanout nets??
                    BelId dstBel = user.cell->bel;
                    if (dstBel == BelId())
                        continue;
                    if (ctx->estimateDelay(ctx->getBelPinWire(cell->bel, port.first),
                                           ctx->getBelPinWire(dstBel, user.port)) > max_net_delay.at(std::make_pair(user.cell->name, user.port)))
                        return false;
                }
            }

        }
        return true;
    }

    BelId cell_swap_bel(CellInfo *cell, BelId newBel) {
        BelId oldBel = cell->bel;
        CellInfo *other_cell = ctx->getBoundBelCell(newBel);
        NPNR_ASSERT(other_cell == nullptr || other_cell->belStrength <= STRENGTH_WEAK);
        ctx->unbindBel(oldBel);
        if (other_cell != nullptr) {
            ctx->unbindBel(newBel);
            ctx->bindBel(oldBel, other_cell, STRENGTH_WEAK);
        }
        ctx->bindBel(newBel, cell, STRENGTH_WEAK);
        return oldBel;
    }

    // Check that a series of moves are both legal and remain within maximum delay bounds
    // Moves are specified as a vector of pairs <cell, oldBel>
    bool acceptable_move(std::vector<std::pair<CellInfo *, BelId>> &move, bool check_delays = true) {
        for (auto &entry : move) {
            if (!ctx->isBelLocationValid(entry.first->bel))
                return false;
            if (!ctx->isBelLocationValid(entry.second))
                return false;
            if (!check_delays)
                continue;
            if (!check_cell_delay_limits(entry.first))
                return false;
            // We might have swapped another cell onto the original bel. Check this for max delay violations
            // too
            CellInfo *swapped = ctx->getBoundBelCell(entry.second);
            if (swapped != nullptr && !check_cell_delay_limits(swapped))
                return false;
        }
        return true;
    }

    int find_neighbours(CellInfo *cell, IdString prev_cell, int d, bool allow_swap) {
        BelId curr = cell->bel;
        Loc curr_loc = ctx->getBelLocation(curr);
        int found_count = 0;
        for (int dy = -d; dy <= d; dy++) {
            for (int dx = -d; dx <= d; dx++) {
                // Go through all the Bels at this location
                // First, find all bels of the correct type that are either unbound or bound normally
                // Strongly bound bels are ignored
                // FIXME: This means that we cannot touch carry chains or similar relatively constrained macros
                std::vector<BelId> free_bels_at_loc;
                std::vector<BelId> bound_bels_at_loc;
                for (auto bel : ctx->getBelsByTile(curr_loc.x + dx, curr_loc.y + dy)) {
                    if (ctx->getBelType(bel) != cell->type)
                        continue;
                    CellInfo *bound = ctx->getBoundBelCell(bel);
                    if (bound == nullptr) {
                        free_bels_at_loc.push_back(bel);
                    } else if (bound->belStrength <= STRENGTH_WEAK) {
                        bound_bels_at_loc.push_back(bel);
                    }
                }
                BelId candidate;

                while (!free_bels_at_loc.empty() && !bound_bels_at_loc.empty()) {
                    BelId try_bel;
                    if (!free_bels_at_loc.empty()) {
                        int try_idx = ctx->rng(int(free_bels_at_loc.size()));
                        try_bel = free_bels_at_loc.at(try_idx);
                        free_bels_at_loc.erase(free_bels_at_loc.begin() + try_idx);
                    } else {
                        int try_idx = ctx->rng(int(bound_bels_at_loc.size()));
                        try_bel = bound_bels_at_loc.at(try_idx);
                        bound_bels_at_loc.erase(bound_bels_at_loc.begin() + try_idx);
                    }
                    if (bel_candidate_cells.count(try_bel) && !allow_swap) {
                        // Overlap is only allowed if it is with the previous cell (this is handled by removing those
                        // edges in the graph), or if allow_swap is true to deal with cases where overlap means few neighbours
                        // are identified
                        if (bel_candidate_cells.at(try_bel).size() > 1 || (bel_candidate_cells.at(try_bel).size() == 0 ||
                        *(bel_candidate_cells.at(try_bel).begin()) != prev_cell))
                            continue;
                    }
                    // TODO: what else to check here?
                    candidate = try_bel;
                    break;
                }

                if (candidate != BelId()) {
                    cell_neighbour_bels[cell->name].insert(candidate);
                    bel_candidate_cells[candidate].insert(cell->name);
                    // Work out if we need to delete any overlap
                    std::vector<IdString> overlap;
                    for (auto other : bel_candidate_cells[candidate])
                        if (other != cell->name && other != prev_cell)
                            overlap.push_back(other);
                    if (overlap.size() > 0)
                        NPNR_ASSERT(allow_swap);
                    for (auto ov : overlap) {
                        bel_candidate_cells[candidate].erase(ov);
                        cell_neighbour_bels[ov].erase(candidate);
                    }
                }
            }
        }
        return found_count;
    }

    std::vector<std::vector<PortRef*>> find_crit_paths(float crit_thresh, size_t max_count) {
        std::vector<std::vector<PortRef*>> crit_paths;
        std::vector<std::pair<NetInfo *, int>> crit_nets;
        std::vector<IdString> netnames;
        std::transform(ctx->nets.begin(), ctx->nets.end(), std::back_inserter(netnames),
                [](const std::pair<const IdString, std::unique_ptr<NetInfo>> &kv){
            return kv.first;
        });
        ctx->sorted_shuffle(netnames);
        for (auto net : netnames) {
            if (crit_nets.size() >= max_count)
                break;
            if (!net_crit.count(net))
                continue;
            auto crit_user = std::max_element(net_crit[net].criticality.begin(),
                    net_crit[net].criticality.end());
            if (*crit_user > crit_thresh)
                crit_nets.push_back(std::make_pair(ctx->nets[net].get(), crit_user - net_crit[net].criticality.begin()));
        }

        auto port_user_index = [](CellInfo *cell, PortInfo &port) -> size_t {
            NPNR_ASSERT(port.net != nullptr);
            for (size_t i = 0; i < port.net->users.size(); i++) {
                auto &usr = port.net->users.at(i);
                if (usr.cell == cell && usr.port == port.name)
                    return i;
            }
            NPNR_ASSERT_FALSE("port user not found on net");
        };

        for (auto crit_net : crit_nets) {
            std::deque<PortRef*> crit_path;

            // FIXME: This will fail badly on combinational loops

            // Iterate backwards following greatest criticality
            NetInfo* back_cursor = crit_net.first;
            while (back_cursor != nullptr) {
                float max_crit = 0;
                std::pair<NetInfo *, size_t> crit_sink{nullptr, 0};
                CellInfo *cell = back_cursor->driver.cell;
                if (cell == nullptr)
                    break;
                for (auto port : cell->ports) {
                    if (port.second.type != PORT_IN)
                        continue;
                    NetInfo *pn = port.second.net;
                    if (pn == nullptr)
                        continue;
                    if (!net_crit.count(pn->name) || net_crit.at(pn->name).criticality.empty())
                        continue;
                    int ccount;
                    DelayInfo combDelay;
                    TimingPortClass tpclass = ctx->getPortTimingClass(cell, port.first, ccount);
                    if (tpclass != TMG_COMB_INPUT)
                        continue;
                    bool is_path = ctx->getCellDelay(cell, port.first, back_cursor->driver.port, combDelay);
                    if (!is_path)
                        continue;
                    size_t user_idx = port_user_index(cell, port.second);
                    float usr_crit = net_crit.at(pn->name).criticality.at(user_idx);
                    if (usr_crit >= max_crit) {
                        max_crit = usr_crit;
                        crit_sink = std::make_pair(pn, user_idx);
                    }
                }

                if (crit_sink.first != nullptr) {
                    crit_path.push_front(&(crit_sink.first->users.at(crit_sink.second)));
                }
                back_cursor = crit_sink.first;
            }
            // Iterate forwards following greatest criticiality
            PortRef *fwd_cursor = &(crit_net.first->users.at(crit_net.second));
            while (fwd_cursor != nullptr) {
                crit_path.push_back(fwd_cursor);
                float max_crit = 0;
                std::pair<NetInfo *, size_t> crit_sink{nullptr, 0};
                CellInfo *cell = fwd_cursor->cell;
                for (auto port : cell->ports) {
                    if (port.second.type != PORT_OUT)
                        continue;
                    NetInfo *pn = port.second.net;
                    if (pn == nullptr)
                        continue;
                    if (!net_crit.count(pn->name) || net_crit.at(pn->name).criticality.empty())
                        continue;
                    int ccount;
                    DelayInfo combDelay;
                    TimingPortClass tpclass = ctx->getPortTimingClass(cell, port.first, ccount);
                    if (tpclass != TMG_COMB_OUTPUT)
                        continue;
                    auto &crits = net_crit.at(pn->name).criticality;
                    auto most_crit_usr = std::max_element(crits.begin(), crits.end());
                    if (*most_crit_usr >= max_crit) {
                        max_crit = *most_crit_usr;
                        crit_sink = std::make_pair(pn, std::distance(crits.begin(), most_crit_usr));
                    }
                }
                if (crit_sink.first != nullptr) {
                    fwd_cursor = &(crit_sink.first->users.at(crit_sink.second));
                } else {
                    fwd_cursor = nullptr;
                }
            }

            std::vector<PortRef*> crit_path_vec;
            std::copy(crit_path.begin(), crit_path.end(), std::back_inserter(crit_path_vec));
            crit_paths.push_back(crit_path_vec);
        }

        return crit_paths;
    }

    void optimise_path(std::vector<PortRef*> &path) {
        path_cells.clear();
        cell_neighbour_bels.clear();
        bel_candidate_cells.clear();
        if (ctx->debug)
            log_info("Optimising the following path: \n");
        for (auto port : path) {
            if (ctx->debug)
                log_info("    %s.%s at %s\n", port->cell->name.c_str(ctx), port->port.c_str(ctx), ctx->getBelName(port->cell->bel).c_str(ctx));
            if (std::find(path_cells.begin(), path_cells.end(), port->cell->name) != path_cells.end())
                continue;
            if (port->cell->belStrength > STRENGTH_WEAK || !cfg.cellTypes.count(port->cell->type))
                continue;
            if (ctx->debug)
                log_info("        can move\n");
            path_cells.push_back(port->cell->name);
        }

        if (path_cells.empty())
            return;

        IdString last_cell;
        const int d = 3; // FIXME: how to best determine d
        for (auto cell : path_cells) {
            // FIXME: when should we allow swapping due to a lack of candidates
            find_neighbours(ctx->cells[cell].get(), last_cell, d, false);
            last_cell = cell;
        }
        // Map cells that we will actually modify to the arc we will use for cost
        // calculation
        // for delay calc purposes
        std::unordered_map<IdString, std::pair<PortRef *, PortRef *>> cost_ports;
        PortRef *last_port = nullptr;
        auto pcell = path_cells.begin();
        for (auto port : path) {
            if (port->cell->name == *pcell) {
                cost_ports[*pcell] = std::make_pair(last_port, port);
                pcell++;
            }
            last_port = port;
        }

        // Actual BFS path optimisation algorithm
        std::unordered_map<IdString, std::unordered_map<BelId, delay_t>> cumul_costs;
        std::unordered_map<std::pair<IdString, BelId>, std::pair<IdString, BelId>> backtrace;
        std::queue<std::pair<int, BelId>> visit;
        std::unordered_set<std::pair<int, BelId>> to_visit;

        for (auto startbel : cell_neighbour_bels[path_cells.front()]) {
            auto entry = std::make_pair(0, startbel);
            visit.push(entry);
            cumul_costs[path_cells.front()][startbel] = 0;
        }

        while(!visit.empty()) {
            auto entry = visit.front();
            visit.pop();
            auto cellname = path_cells.at(entry.first);
            if (entry.first == int(path_cells.size()) - 1)
                continue;
            std::vector<std::pair<CellInfo *, BelId>> move;
            // Apply the entire backtrace for accurate legality and delay checks
            // This is probably pretty expensive (but also probably pales in comparison to the number of swaps
            // SA will make...)
            std::vector<std::pair<IdString, BelId>> route_to_entry;
            auto cursor = std::make_pair(cellname, entry.second);
            route_to_entry.push_back(cursor);
            while (backtrace.count(cursor)) {
                cursor = backtrace.at(cursor);
                route_to_entry.push_back(cursor);
            }
            for (auto rt_entry : boost::adaptors::reverse(route_to_entry)) {
                CellInfo *cell = ctx->cells.at(rt_entry.first).get();
                BelId origBel = cell_swap_bel(cell, rt_entry.second);
                move.push_back(std::make_pair(cell, origBel));
            }

            delay_t cdelay = cumul_costs[cellname][entry.second];

            // Have a look at where we can travel from here
            for (auto neighbour : cell_neighbour_bels.at(path_cells.at(entry.first + 1))) {
                // Edges between overlapping bels are deleted
                if (neighbour == entry.second)
                    continue;
                // Experimentally swap the next path cell onto the neighbour bel we are trying
                IdString ncname = path_cells.at(entry.first + 1);
                CellInfo *next_cell = ctx->cells.at(ncname).get();
                BelId origBel = cell_swap_bel(next_cell, neighbour);
                move.push_back(std::make_pair(next_cell, origBel));

                // Check the new cumulative delay
                auto port_pair = cost_ports.at(ncname);
                delay_t edge_delay = ctx->estimateDelay(ctx->getBelPinWire(port_pair.first->cell->bel, port_pair.first->port),
                                                        ctx->getBelPinWire(port_pair.second->cell->bel, port_pair.second->port));
                delay_t total_delay = cdelay + edge_delay;
                // First, check if the move is actually worthwhile from a delay point of view before the expensive
                // legality check
                if (!cumul_costs.count(ncname) || !cumul_costs.at(ncname).count(neighbour)
                    || cumul_costs.at(ncname).at(neighbour) > total_delay) {
                    // Now check that the swaps we have made to get here are legal and meet max delay requirements
                    if (acceptable_move(move)) {
                        cumul_costs[ncname][neighbour] = total_delay;
                        backtrace[std::make_pair(ncname, neighbour)] = std::make_pair(cellname, entry.second);
                        if (!to_visit.count(std::make_pair(entry.first + 1, neighbour)))
                            visit.push(std::make_pair(entry.first + 1, neighbour));
                    }
                }
                // Revert the experimental swap
                cell_swap_bel(move.back().first, move.back().second);
                move.pop_back();
            }

            // Revert move by swapping cells back to their original order
            // Execute swaps in reverse order to how we made them originally
            for (auto move_entry : boost::adaptors::reverse(move)) {
                cell_swap_bel(move_entry.first, move_entry.second);
            }
        }

        // Did we find a solution??
        if (cumul_costs.count(path_cells.back())) {
            // Find the end position with the lowest total delay
            auto &end_options = cumul_costs.at(path_cells.back());
            auto lowest = std::min_element(end_options.begin(), end_options.end(), [](const std::pair<BelId, delay_t> &a,
                                                                                      const std::pair<BelId, delay_t> &b) {
               return a.second < b.second;
            });
            NPNR_ASSERT(lowest != end_options.end());

            std::vector<std::pair<IdString, BelId>> route_to_solution;
            auto cursor = std::make_pair(path_cells.back(), lowest->first);
            route_to_solution.push_back(cursor);
            while (backtrace.count(cursor)) {
                cursor = backtrace.at(cursor);
                route_to_solution.push_back(cursor);
            }
            if (ctx->debug)
                log_info("Found a solution with cost %.02f ns\n", ctx->getDelayNS(lowest->second));
            for (auto rt_entry : boost::adaptors::reverse(route_to_solution)) {
                CellInfo *cell = ctx->cells.at(rt_entry.first).get();
                cell_swap_bel(cell, rt_entry.second);
                if(ctx->debug)
                    log_info("    %s at %s\n", rt_entry.first.c_str(ctx), ctx->getBelName(rt_entry.second).c_str(ctx));
            }

        } else {
            if (ctx->debug)
                log_info("Solution was not found\n");
        }
        if (ctx->debug)
            log_break();
    }

    // Current candidate Bels for cells (linked in both direction>
    std::vector<IdString> path_cells;
    std::unordered_map<IdString, std::unordered_set<BelId>> cell_neighbour_bels;
    std::unordered_map<BelId, std::unordered_set<IdString>> bel_candidate_cells;
    // Map cell ports to net delay limit
    std::unordered_map<std::pair<IdString, IdString>, delay_t> max_net_delay;
    // Criticality data from timing analysis
    NetCriticalityMap net_crit;
    Context *ctx;
    TimingOptCfg cfg;

};

bool timing_opt(Context *ctx, TimingOptCfg cfg) { return TimingOptimiser(ctx, cfg).optimise(); }

NEXTPNR_NAMESPACE_END
