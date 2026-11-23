#include "graph/GraphLayout.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>

namespace sg::graph {

size_t ArrangeColumns(std::vector<LayoutBox>& boxes, const std::vector<LayoutEdge>& edges,
                      float columnGap, float rowGap) {
    const size_t count = boxes.size();
    if (count == 0) return 0;
    const std::vector<LayoutBox> original = boxes;

    std::vector<LayoutEdge> valid;
    std::vector<bool> connected(count, false);
    for (const LayoutEdge& edge : edges) {
        if (edge.from >= count || edge.to >= count || edge.from == edge.to) continue;
        valid.push_back(edge);
        connected[edge.from] = true;
        connected[edge.to] = true;
    }

    // --- 列。右端が 0、左へ行くほど大きい。下流のうち最も左のものの 1 つ左 ---
    // 循環が無ければ、箱の数だけ回す前に変化が止まる。
    std::vector<int> column(count, 0);
    for (size_t pass = 0; pass < count; ++pass) {
        bool changed = false;
        for (const LayoutEdge& edge : valid) {
            if (column[edge.from] < column[edge.to] + 1) {
                column[edge.from] = column[edge.to] + 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
    int lastColumn = 0;
    for (size_t i = 0; i < count; ++i) lastColumn = std::max(lastColumn, std::min(column[i], static_cast<int>(count)));
    for (size_t i = 0; i < count; ++i) {
        column[i] = std::min(column[i], lastColumn);
        if (!connected[i]) column[i] = lastColumn;  // 繋がっていない箱は左端
    }

    // --- 行。右の列から順に、繋がる先の高さへ寄せる ---
    struct Item {
        size_t index = 0;
        float target = 0.0f;  // 置きたい中心の高さ
        int slot = 0;         // 繋がる先の入力ピンの番号（同じ先に繋がる箱どうしの順）
    };
    for (int c = 0; c <= lastColumn; ++c) {
        std::vector<Item> items;
        for (size_t i = 0; i < count; ++i) {
            if (column[i] != c) continue;
            Item item{i, original[i].y + original[i].height * 0.5f, 0};
            float sum = 0.0f;
            int sinks = 0;
            int slot = INT_MAX;
            for (const LayoutEdge& edge : valid) {
                // すでに置いた列（右側）の箱だけを見る。
                if (edge.from != i || column[edge.to] >= c) continue;
                sum += boxes[edge.to].y + boxes[edge.to].height * 0.5f;
                slot = std::min(slot, edge.toSlot);
                ++sinks;
            }
            if (sinks > 0) {
                item.target = sum / static_cast<float>(sinks);
                item.slot = slot;
            } else if (!connected[i]) {
                item.target = FLT_MAX;  // 繋がっていない箱は列の下
            }
            items.push_back(item);
        }
        std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
            if (a.target != b.target) return a.target < b.target;
            if (a.slot != b.slot) return a.slot < b.slot;
            return original[a.index].y < original[b.index].y;
        });

        // 上から詰めて置き、置きたい高さとのずれの平均が 0 になるよう、列ごと上下にずらす。
        float cursor = -FLT_MAX;
        float drift = 0.0f;
        int drifted = 0;
        for (const Item& item : items) {
            LayoutBox& box = boxes[item.index];
            const bool first = (cursor == -FLT_MAX);
            if (item.target == FLT_MAX) {
                box.y = first ? original[item.index].y : cursor;
            } else {
                const float wanted = item.target - box.height * 0.5f;
                box.y = first ? wanted : std::max(wanted, cursor);
                drift += box.y - wanted;
                ++drifted;
            }
            cursor = box.y + box.height + rowGap;
        }
        if (drifted > 0) {
            const float shift = drift / static_cast<float>(drifted);
            for (const Item& item : items) boxes[item.index].y -= shift;
        }
    }

    // --- 横。列の幅は、その列で最も広い箱 ---
    std::vector<float> width(static_cast<size_t>(lastColumn) + 1, 0.0f);
    for (size_t i = 0; i < count; ++i) {
        const size_t c = static_cast<size_t>(column[i]);
        width[c] = std::max(width[c], boxes[i].width);
    }
    std::vector<float> left(width.size(), 0.0f);
    float x = 0.0f;
    for (size_t c = width.size(); c-- > 0;) {
        left[c] = x;
        x += width[c] + columnGap;
    }
    for (size_t i = 0; i < count; ++i) boxes[i].x = left[static_cast<size_t>(column[i])];

    // --- 全体の左上を、並べる前の左上に合わせる ---
    float beforeX = FLT_MAX, beforeY = FLT_MAX, afterX = FLT_MAX, afterY = FLT_MAX;
    for (size_t i = 0; i < count; ++i) {
        beforeX = std::min(beforeX, original[i].x);
        beforeY = std::min(beforeY, original[i].y);
        afterX = std::min(afterX, boxes[i].x);
        afterY = std::min(afterY, boxes[i].y);
    }
    size_t moved = 0;
    for (size_t i = 0; i < count; ++i) {
        // 小数の位置はリンクがにじむので、整数に丸める。
        boxes[i].x = std::round(boxes[i].x + beforeX - afterX);
        boxes[i].y = std::round(boxes[i].y + beforeY - afterY);
        if (boxes[i].x != original[i].x || boxes[i].y != original[i].y) ++moved;
    }
    return moved;
}

}  // namespace sg::graph
