#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "index/bplus_tree.h"

using namespace ocera;

static int failures = 0;
static void Check(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; ++failures; }
}
static RID MakeRid(std::uint32_t page, std::uint32_t slot, std::uint32_t generation = 1) {
    RID rid;
    rid.page_id = static_cast<page_id_t>(page);
    rid.slot_num = slot;
    rid.generation = generation;
    return rid;
}
static void CheckValid(const BPlusTree& tree, const std::string& where) {
    std::string error;
    Check(tree.CheckInvariants(&error), where + ": " + error);
}

int main() {
    {
        BPlusTree tree(3);
        Check(tree.Empty() && tree.Size() == 0, "new tree is empty");
        Check(tree.SearchAll(5).empty(), "SearchAll on empty tree");
        Check(!tree.Remove(5, MakeRid(1, 1)), "remove missing pair");
        CheckValid(tree, "empty tree");
    }
    {
        BPlusTree tree(3);
        const RID a = MakeRid(10, 1), b = MakeRid(10, 2), c = MakeRid(10, 3);
        Check(tree.Insert(20, a), "insert first duplicate key");
        Check(tree.Insert(20, b), "insert second RID for key");
        Check(tree.Insert(20, c), "insert third RID for key");
        Check(!tree.Insert(20, b), "reject exact duplicate pair");
        Check(tree.Size() == 3, "size counts pairs");
        RID first;
        Check(tree.Search(20, &first) && first == a, "Search returns first inserted RID");
        const auto all = tree.SearchAll(20);
        Check(all.size() == 3 && all[0] == a && all[1] == b && all[2] == c, "SearchAll preserves insertion order");
        Check(tree.Remove(20, b), "remove middle RID");
        Check(tree.SearchAll(20).size() == 2, "two RIDs remain");
        Check(!tree.Remove(20, b), "removed pair cannot be removed twice");
        Check(tree.Remove(20, a) && tree.Remove(20, c), "remove remaining RIDs");
        Check(!tree.Search(20, nullptr) && tree.Empty(), "key removed after final RID");
        CheckValid(tree, "duplicate-key tree after deletes");
    }
    {
        BPlusTree tree(3);
        for (std::int64_t k = 0; k < 300; ++k) {
            Check(tree.Insert(k, MakeRid(static_cast<std::uint32_t>(k + 1), 0)), "ascending insert");
            CheckValid(tree, "after ascending insert");
        }
        for (std::int64_t k = 0; k < 300; ++k) {
            Check(tree.Remove(k, MakeRid(static_cast<std::uint32_t>(k + 1), 0)), "ascending delete");
            CheckValid(tree, "after ascending delete");
        }
        Check(tree.Empty() && tree.Height() == 1, "tree shrinks back to empty leaf root");
    }
    {
        BPlusTree tree(4);
        std::map<std::int64_t, std::vector<RID>> reference;
        std::mt19937 rng(123456);
        for (int i = 0; i < 4000; ++i) {
            const std::int64_t key = static_cast<int>(rng() % 180) - 90;
            const RID rid = MakeRid(static_cast<std::uint32_t>(i + 1), static_cast<std::uint32_t>(i % 17), 1);
            if ((rng() & 1U) == 0) {
                const bool expected = std::find(reference[key].begin(), reference[key].end(), rid) == reference[key].end();
                Check(tree.Insert(key, rid) == expected, "random insert result");
                if (expected) reference[key].push_back(rid);
            } else {
                auto it = reference.find(key);
                bool expected = false;
                if (it != reference.end()) {
                    auto rit = std::find(it->second.begin(), it->second.end(), rid);
                    if (rit != it->second.end()) { it->second.erase(rit); expected = true; }
                    if (it->second.empty()) reference.erase(it);
                }
                Check(tree.Remove(key, rid) == expected, "random remove result");
            }
            std::size_t total = 0;
            for (const auto& entry : reference) total += entry.second.size();
            Check(tree.Size() == total, "random size matches reference");
            CheckValid(tree, "random mutation");
        }
        std::vector<std::pair<std::int64_t, RID>> expected;
        for (const auto& entry : reference)
            for (const RID& rid : entry.second) expected.emplace_back(entry.first, rid);
        const auto actual = tree.RangeScan(std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max());
        Check(actual.size() == expected.size(), "random full range size");
        Check(actual == expected, "random full range contents and ordering");
    }
    {
        BPlusTree tree(3);
        const auto lo = std::numeric_limits<std::int64_t>::min();
        const auto hi = std::numeric_limits<std::int64_t>::max();
        Check(tree.Insert(lo, MakeRid(1, 1)), "insert INT64_MIN");
        Check(tree.Insert(hi, MakeRid(2, 2)), "insert INT64_MAX");
        const auto all = tree.RangeScan(lo, hi);
        Check(all.size() == 2 && all.front().first == lo && all.back().first == hi, "range handles integer extremes");
        CheckValid(tree, "extreme-key tree");
    }

    {
        // Descending insertion exercises splits on the left edge of the tree.
        BPlusTree tree(3);
        for (std::int64_t k = 299; k >= 0; --k) {
            Check(tree.Insert(k, MakeRid(static_cast<std::uint32_t>(k + 1), 1)),
                  "descending insert");
        }
        Check(tree.Height() > 1, "descending inserts grow tree height");
        CheckValid(tree, "after descending inserts");

        RID found;
        Check(tree.Search(0, &found), "search smallest descending-inserted key");
        Check(tree.Search(299, &found), "search largest descending-inserted key");
        Check(!tree.Search(-1, nullptr), "missing key below range");
        Check(!tree.Search(300, nullptr), "missing key above range");

        const auto middle = tree.RangeScan(100, 105);
        Check(middle.size() == 6, "inclusive range scan returns both boundaries");
        Check(!middle.empty() && middle.front().first == 100 &&
              middle.back().first == 105, "range scan boundary keys");
        Check(tree.RangeScan(106, 100).empty(), "reversed range is empty");

        const auto single = tree.RangeScan(150, 150);
        Check(single.size() == 1 && single.front().first == 150,
              "single-key inclusive range scan");
        CheckValid(tree, "descending tree");
    }
    {
        // Constructor rejects fanout values that cannot support the algorithms.
        bool rejected_small = false;
        bool rejected_negative = false;
        try { BPlusTree invalid(2); }
        catch (const std::invalid_argument&) { rejected_small = true; }
        try { BPlusTree invalid(-1); }
        catch (const std::invalid_argument&) { rejected_negative = true; }
        Check(rejected_small, "constructor rejects max_keys below three");
        Check(rejected_negative, "constructor rejects negative max_keys");
    }
    {
        // A larger deterministic workload checks height growth and lookup
        // without running the expensive invariant walk after every insertion.
        BPlusTree tree(8);
        constexpr std::int64_t count = 20000;
        for (std::int64_t k = 0; k < count; ++k) {
            Check(tree.Insert(k, MakeRid(static_cast<std::uint32_t>(k + 1), 2)),
                  "large ascending insert");
        }
        Check(tree.Height() > 1, "large workload grows tree height");
        Check(tree.Size() == static_cast<std::size_t>(count),
              "large workload pair count");
        RID found;
        Check(tree.Search(0, &found), "large workload finds first key");
        Check(tree.Search(count / 2, &found), "large workload finds middle key");
        Check(tree.Search(count - 1, &found), "large workload finds final key");
        Check(!tree.Search(count, nullptr), "large workload rejects missing key");
        const auto range = tree.RangeScan(9990, 10009);
        Check(range.size() == 20, "large workload bounded range size");
        Check(!range.empty() && range.front().first == 9990 &&
              range.back().first == 10009, "large workload bounded range endpoints");
        CheckValid(tree, "large workload");
    }
    if (failures == 0) { std::cout << "All B+Tree tests passed.\n"; return 0; }
    std::cerr << failures << " B+Tree test(s) failed.\n";
    return 1;
}
