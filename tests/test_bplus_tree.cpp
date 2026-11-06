#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "index/bplus_tree.h"

using namespace ocera;

static int g_failures = 0;

static void Check(bool cond, const std::string& what) {
    if (!cond) {
        std::cout << "    FAILED: " << what << "\n";
        ++g_failures;
    }
}

// A predictable RID for each key, so tests can confirm the RID came back intact.
static RID MakeRid(std::int64_t k) {
    // Unsigned math avoids overflow for extreme keys such as INT64_MIN.
    std::uint64_t u = static_cast<std::uint64_t>(k);
    RID r;
    r.page_id = static_cast<page_id_t>(u % 100000);
    r.slot_num = static_cast<std::uint32_t>((u * 7) % 1000);
    return r;
}

static void CheckTree(const BPlusTree& t, const std::string& ctx) {
    std::string err;
    Check(t.CheckInvariants(&err), ctx + ": invariants broken: " + err);
}

static void CheckFound(const BPlusTree& t, std::int64_t k) {
    RID r;
    bool found = t.Search(k, &r);
    Check(found, "key " + std::to_string(k) + " should be found");
    if (found) Check(r == MakeRid(k), "RID for key " + std::to_string(k) + " is wrong");
}

static void Report(const char* name, int before) {
    std::cout << (g_failures == before ? "    PASSED: " : "    (see failures above) ") << name << "\n";
}

int main() {
    std::cout << "=== OCERA B+Tree Test (in-memory version) ===\n\n";
    int before;

    std::cout << "[1] Empty tree\n";
    before = g_failures;
    {
        BPlusTree t(3);
        RID r;
        Check(t.Empty() && t.Size() == 0, "empty tree has size 0");
        Check(!t.Search(5, &r), "search in empty tree finds nothing");
        Check(t.RangeScan(0, 100).empty(), "range scan of empty tree is empty");
        Check(t.Height() == 1, "empty tree height is 1");
        CheckTree(t, "empty");
    }
    Report("empty tree", before);

    std::cout << "[2] Single key\n";
    before = g_failures;
    {
        BPlusTree t(3);
        Check(t.Insert(42, MakeRid(42)), "insert 42 succeeds");
        Check(t.Size() == 1, "size is 1");
        CheckFound(t, 42);
        Check(!t.Search(41, nullptr) && !t.Search(43, nullptr), "neighbours not found");
        CheckTree(t, "single");
    }
    Report("single key", before);

    std::cout << "[3] Ascending inserts 1..200 (max_keys=3), invariants after every insert\n";
    before = g_failures;
    {
        BPlusTree t(3);
        for (int k = 1; k <= 200; ++k) {
            Check(t.Insert(k, MakeRid(k)), "insert " + std::to_string(k));
            CheckTree(t, "ascending after " + std::to_string(k));
        }
        for (int k = 1; k <= 200; ++k) CheckFound(t, k);
        Check(t.Size() == 200, "size is 200");
    }
    Report("ascending", before);

    std::cout << "[4] Descending inserts 200..1 (max_keys=3)\n";
    before = g_failures;
    {
        BPlusTree t(3);
        for (int k = 200; k >= 1; --k) {
            t.Insert(k, MakeRid(k));
            CheckTree(t, "descending after " + std::to_string(k));
        }
        for (int k = 1; k <= 200; ++k) CheckFound(t, k);
        Check(t.Size() == 200, "size is 200");
    }
    Report("descending", before);

    std::cout << "[5] Random inserts compared against std::map, for several node sizes\n";
    before = g_failures;
    for (int m : {3, 4, 5, 8, 64}) {
        std::mt19937 rng(12345 + m);
        std::vector<int> keys(1500);
        std::iota(keys.begin(), keys.end(), -700);
        std::shuffle(keys.begin(), keys.end(), rng);
        BPlusTree t(m);
        std::map<std::int64_t, RID> ref;
        for (int k : keys) {
            t.Insert(k, MakeRid(k));
            ref[k] = MakeRid(k);
        }
        CheckTree(t, "random max_keys=" + std::to_string(m));
        Check(t.Size() == ref.size(), "size matches reference");
        for (auto& kv : ref) CheckFound(t, kv.first);
        auto all = t.RangeScan(std::numeric_limits<std::int64_t>::min(),
                               std::numeric_limits<std::int64_t>::max());
        Check(all.size() == ref.size(), "full scan returns every key");
        bool ordered = true;
        std::size_t i = 0;
        for (auto& kv : ref) {
            if (i >= all.size() || all[i].first != kv.first || !(all[i].second == kv.second)) ordered = false;
            ++i;
        }
        Check(ordered, "full scan matches reference order and RIDs");
    }
    Report("random vs std::map", before);

    std::cout << "[6] Missing keys (gaps, below the minimum, above the maximum)\n";
    before = g_failures;
    {
        BPlusTree t(4);
        for (int k = 0; k < 500; k += 2) t.Insert(k, MakeRid(k));  // evens only
        for (int k = 1; k < 500; k += 2) Check(!t.Search(k, nullptr), "odd key " + std::to_string(k) + " should be missing");
        Check(!t.Search(-1, nullptr), "below minimum");
        Check(!t.Search(100000, nullptr), "above maximum");
        CheckTree(t, "missing keys");
    }
    Report("missing keys", before);

    std::cout << "[7] Duplicate keys are rejected and change nothing\n";
    before = g_failures;
    {
        BPlusTree t(3);
        for (int k = 1; k <= 50; ++k) t.Insert(k, MakeRid(k));
        RID other{999, 999};
        Check(!t.Insert(25, other), "duplicate insert returns false");
        Check(t.Size() == 50, "size unchanged after duplicate");
        CheckFound(t, 25);  // still the ORIGINAL RID
        Check(!t.Insert(1, other) && !t.Insert(50, other), "duplicates at the edges rejected");
        CheckTree(t, "duplicates");
    }
    Report("duplicates", before);

    std::cout << "[8] Splits and root changes make the tree grow taller\n";
    before = g_failures;
    {
        BPlusTree t(3);
        for (int k = 1; k <= 3; ++k) t.Insert(k, MakeRid(k));
        Check(t.Height() == 1, "3 keys still fit in one leaf (height 1)");
        t.Insert(4, MakeRid(4));
        Check(t.Height() == 2, "4th key splits the leaf and creates a new root (height 2)");
        int last = t.Height();
        for (int k = 5; k <= 60; ++k) {
            t.Insert(k, MakeRid(k));
            Check(t.Height() >= last, "height never shrinks");
            last = t.Height();
        }
        Check(t.Height() >= 3, "internal node splits raise height to at least 3");
        CheckTree(t, "root changes");
        for (int k = 1; k <= 60; ++k) CheckFound(t, k);
    }
    Report("splits / root changes", before);

    std::cout << "[9] Range scans\n";
    before = g_failures;
    {
        BPlusTree t(3);
        for (int k = 10; k <= 1000; k += 10) t.Insert(k, MakeRid(k));  // 10,20,...,1000
        auto r = t.RangeScan(100, 200);
        Check(r.size() == 11 && r.front().first == 100 && r.back().first == 200, "bounds are inclusive");
        r = t.RangeScan(105, 195);
        Check(r.size() == 9 && r.front().first == 110 && r.back().first == 190, "bounds that are not keys");
        Check(t.RangeScan(500, 500).size() == 1, "single-key range");
        Check(t.RangeScan(501, 509).empty(), "range inside a gap is empty");
        Check(t.RangeScan(200, 100).empty(), "low > high is empty");
        Check(t.RangeScan(-50, 5).empty(), "range entirely below keys");
        Check(t.RangeScan(2000, 3000).empty(), "range entirely above keys");
        Check(t.RangeScan(-50, 25).size() == 2, "range starting below minimum");
        Check(t.RangeScan(990, 5000).size() == 2, "range running past maximum");
        r = t.RangeScan(0, 100000);
        bool sorted = std::is_sorted(r.begin(), r.end(),
                                     [](const auto& a, const auto& b) { return a.first < b.first; });
        Check(r.size() == 100 && sorted, "whole-range scan is complete and sorted (crosses many leaves)");
    }
    Report("range scans", before);

    std::cout << "[10] Larger tree: 20000 random keys, max_keys=32\n";
    before = g_failures;
    {
        std::mt19937 rng(777);
        std::vector<int> keys(20000);
        std::iota(keys.begin(), keys.end(), 0);
        std::shuffle(keys.begin(), keys.end(), rng);
        BPlusTree t(32);
        for (int k : keys) t.Insert(k, MakeRid(k));
        CheckTree(t, "large");
        Check(t.Size() == 20000, "size is 20000");
        for (int k = 0; k < 20000; k += 7) CheckFound(t, k);
        Check(t.RangeScan(5000, 5999).size() == 1000, "range of 1000 keys");
        std::cout << "    (height of this tree: " << t.Height() << ")\n";
    }
    Report("large tree", before);

    std::cout << "[11] Negative keys and int64 extremes\n";
    before = g_failures;
    {
        BPlusTree t(3);
        const std::int64_t lo = std::numeric_limits<std::int64_t>::min();
        const std::int64_t hi = std::numeric_limits<std::int64_t>::max();
        for (std::int64_t k : {lo, hi, std::int64_t(0), std::int64_t(-1), std::int64_t(1), lo + 1, hi - 1})
            Check(t.Insert(k, MakeRid(k)), "insert extreme key");
        for (std::int64_t k : {lo, hi, std::int64_t(0), std::int64_t(-1), std::int64_t(1), lo + 1, hi - 1})
            CheckFound(t, k);
        auto all = t.RangeScan(lo, hi);
        Check(all.size() == 7 && all.front().first == lo && all.back().first == hi, "full range with extremes");
        CheckTree(t, "extremes");
    }
    Report("extremes", before);

    std::cout << "[12] Invalid node size is rejected\n";
    before = g_failures;
    {
        bool threw = false;
        try { BPlusTree t(2); } catch (const std::invalid_argument&) { threw = true; }
        Check(threw, "max_keys < 3 throws std::invalid_argument");
    }
    Report("invalid max_keys", before);

    if (g_failures == 0) {
        std::cout << "\nAll B+Tree tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " B+Tree check(s) FAILED.\n";
    return 1;
}
