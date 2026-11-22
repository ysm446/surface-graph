# テスト用グラフとサンプルのグラフを生成する。
#
#   python tools/make_test_graphs.py
#
# graphs/tests/all_nodes.sgraph  全ノードを 1 回ずつ通す（CLI の check / golden 用）
# graphs/brick.sgraph            レンガのマテリアル（6 チャンネルの出力）
#
# ノードを足したら、ここの all_nodes にも 1 つ足して、CLI で画像を確かめること。
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Graph:
    def __init__(self, name, resolution=1024, seed=0):
        self.data = {"format": "surface-graph", "version": 1, "name": name, "resolution": resolution,
                     "seed": seed, "nodes": [], "links": [], "exposedParams": []}
        self.next_id = 1
        self.column = {}

    def node(self, type_, column, params=None, inputs=None, seed_offset=0):
        """inputs は {入力ピン名: ノード ID か (ノード ID, 出力ピン名)}。"""
        node_id = self.next_id
        self.next_id += 1
        row = self.column.get(column, 0)
        self.column[column] = row + 1
        item = {"id": node_id, "type": type_, "pos": [column * 320, row * 260], "params": params or {}}
        if seed_offset:
            item["seedOffset"] = seed_offset
        self.data["nodes"].append(item)
        for pin, source in (inputs or {}).items():
            source_id, source_pin = source if isinstance(source, tuple) else (source, "Out")
            self.data["links"].append({"from": source_id, "fromPin": source_pin, "to": node_id, "toPin": pin})
        return node_id

    def save(self, relative):
        path = os.path.join(ROOT, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(self.data, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        print(relative, len(self.data["nodes"]), "nodes")


def all_nodes():
    g = Graph("all_nodes")
    perlin = g.node("noise.perlin", 0, {"scale": 4, "octaves": 5})
    ridged = g.node("noise.perlin", 0, {"scale": 3, "octaves": 4, "mode": 1}, seed_offset=1)
    worley = g.node("noise.worley", 0, {"scale": 8, "mode": 0})
    cells = g.node("noise.worley", 0, {"scale": 6, "mode": 3}, seed_offset=2)
    borders = g.node("noise.worley", 0, {"scale": 6, "mode": 2}, seed_offset=2)
    value = g.node("noise.value", 0, {"scale": 8, "octaves": 3})
    white = g.node("noise.white", 0, {"scale": 64})

    shape = g.node("shape.shape", 1, {"shape": 0, "size": [0.8, 0.8], "bevel": 0.3, "corner": 0.2})
    polygon = g.node("shape.shape", 1, {"shape": 2, "size": [0.7, 0.7], "sides": 6, "bevel": 0.0})
    disc = g.node("shape.shape", 1, {"shape": 1, "size": [0.6, 0.6], "bevel": 1.0})
    gradient = g.node("shape.gradient", 1, {"mode": 1})
    color = g.node("shape.uniform_color", 1, {"color": [0.8, 0.35, 0.2, 1.0]})
    brick = g.node("pattern.brick", 1, {"columns": 4, "rows": 8, "bevel": 0.2, "luminanceRandom": 0.3})
    checker = g.node("pattern.checker", 1, {"tiles": 4})

    tiles = g.node("pattern.tile_generator", 2,
                   {"columns": 6, "rows": 6, "scale": 0.9, "rotationRandom": 0.1, "luminanceRandom": 0.5,
                    "positionRandom": 0.2}, {"Pattern": shape})
    tiles_default = g.node("pattern.tile_generator", 2, {"columns": 5, "rows": 10, "offset": 0.5, "scale": 0.85})
    levels = g.node("adjust.levels", 2, {"inLow": 0.3, "inHigh": 0.7}, {"In": perlin})
    invert = g.node("adjust.invert", 2, None, {"In": worley})
    scan = g.node("adjust.histogram_scan", 2, {"position": 0.5, "contrast": 0.9}, {"In": perlin})
    threshold = g.node("adjust.threshold", 2, {"threshold": 0.5}, {"In": value})
    g.node("adjust.clamp", 2, {"low": 0.2, "high": 0.6}, {"In": gradient})
    g.node("adjust.histogram_range", 2, {"range": 0.3, "position": 0.8}, {"In": perlin})

    blur = g.node("filter.blur", 3, {"radius": 0.02}, {"In": checker})
    g.node("filter.directional_blur", 3, {"radius": 0.03, "angle": 0.1}, {"In": brick})
    warp = g.node("filter.warp", 3, {"intensity": 0.05}, {"In": brick, "Gradient": perlin})
    g.node("filter.directional_warp", 3, {"intensity": 0.05, "angle": 0.125}, {"In": checker, "Intensity": perlin})
    g.node("filter.slope_blur", 3, {"intensity": 0.05, "samples": 16, "mode": 1}, {"In": tiles, "Slope": ridged})
    g.node("filter.edge_detect", 3, {"width": 0.004, "intensity": 2.0}, {"In": tiles_default})
    g.node("filter.transform", 3, {"offset": [0.25, 0.1], "rotation": 0.25, "tiling": 2}, {"In": brick})
    g.node("filter.distance", 3, {"distance": 0.08}, {"In": threshold})

    blend = g.node("blend.blend", 4, {"mode": 3, "opacity": 1.0}, {"Foreground": perlin, "Background": warp})
    g.node("blend.blend", 4, {"mode": 0, "opacity": 0.8}, {"Foreground": cells, "Background": gradient, "Mask": scan})
    g.node("blend.height_blend", 4, {"offset": 0.45, "contrast": 0.9}, {"HeightA": warp, "HeightB": levels})

    gradient_map = g.node("color.gradient_map", 5, {"gradient": [
        {"pos": 0.0, "color": [0.20, 0.10, 0.06, 1.0]},
        {"pos": 0.5, "color": [0.55, 0.25, 0.15, 1.0]},
        {"pos": 1.0, "color": [0.80, 0.55, 0.40, 1.0]}]}, {"In": blend})
    hsl = g.node("color.hsl", 5, {"hue": 0.6, "saturation": 0.6, "lightness": 0.5}, {"In": gradient_map})
    g.node("color.grayscale", 5, {"channel": 0}, {"In": hsl})
    split = g.node("color.channel_split", 5, None, {"In": gradient_map})
    g.node("color.channel_merge", 5, None, {"R": (split, "B"), "G": (split, "G"), "B": (split, "R")})
    g.node("blend.blend", 5, {"mode": 0, "opacity": 0.5}, {"Foreground": color, "Background": gradient_map, "Mask": borders})
    g.node("filter.blur", 5, {"radius": 0.01}, {"In": gradient_map})

    normal = g.node("derive.normal", 6, {"intensity": 0.05, "format": 0}, {"In": warp})
    g.node("derive.ao", 6, {"heightScale": 0.1, "radius": 0.05}, {"In": warp})
    g.node("derive.curvature", 6, {"intensity": 1.0, "width": 0.002}, {"In": normal})
    g.node("io.output", 6, {"usage": 1}, {"In": normal})
    g.node("io.output", 6, {"usage": 0}, {"In": gradient_map})
    g.node("io.output", 6, {"usage": 4}, {"In": blur})
    g.node("adjust.invert", 6, None, {"In": disc})
    g.node("adjust.invert", 6, None, {"In": polygon})
    g.node("adjust.invert", 6, None, {"In": invert})
    g.node("adjust.levels", 6, {"gamma": 2.0}, {"In": white})
    g.save("graphs/tests/all_nodes.sgraph")


def brick():
    g = Graph("brick", resolution=2048)
    # 形: レンガを並べ、ノイズで縁を崩す。
    bricks = g.node("pattern.brick", 0, {"columns": 4, "rows": 8, "offset": 0.5, "gap": 0.08, "bevel": 0.18,
                                         "luminanceRandom": 0.25})
    warp_noise = g.node("noise.perlin", 0, {"scale": 6, "octaves": 4}, seed_offset=1)
    surface_noise = g.node("noise.perlin", 0, {"scale": 16, "octaves": 6, "gain": 0.6}, seed_offset=2)
    pits = g.node("noise.worley", 0, {"scale": 24, "mode": 0}, seed_offset=3)
    grime = g.node("noise.perlin", 0, {"scale": 3, "octaves": 6, "gain": 0.6}, seed_offset=4)

    warped = g.node("filter.warp", 1, {"intensity": 0.012}, {"In": bricks, "Gradient": warp_noise})
    random_warped = g.node("filter.warp", 1, {"intensity": 0.012}, {"In": (bricks, "Random"), "Gradient": warp_noise})
    eroded = g.node("filter.slope_blur", 1, {"intensity": 0.02, "samples": 12, "mode": 1},
                    {"In": warped, "Slope": surface_noise})

    # 高さ: 表面の細かい凹凸と、点状の欠けを足す。
    rough = g.node("blend.blend", 2, {"mode": 3, "opacity": 0.25}, {"Foreground": surface_noise, "Background": eroded})
    pits_mask = g.node("adjust.histogram_scan", 2, {"position": 0.12, "contrast": 0.7}, {"In": pits})
    pitted = g.node("blend.blend", 2, {"mode": 2, "opacity": 0.12}, {"Foreground": pits_mask, "Background": rough})
    height = g.node("adjust.levels", 2, {"inLow": 0.0, "inHigh": 1.0, "gamma": 1.0}, {"In": pitted})
    brick_mask = g.node("adjust.histogram_scan", 2, {"position": 0.75, "contrast": 0.85}, {"In": eroded})

    # 導出: 法線・AO・曲率。
    normal = g.node("derive.normal", 3, {"intensity": 0.035, "format": 0}, {"In": height})
    ao = g.node("derive.ao", 3, {"heightScale": 0.06, "radius": 0.03}, {"In": height})
    curvature = g.node("derive.curvature", 3, {"intensity": 1.5, "width": 0.002}, {"In": normal})
    edge_wear = g.node("adjust.histogram_scan", 3, {"position": 0.38, "contrast": 0.8}, {"In": curvature})
    cavity = g.node("adjust.levels", 3, {"inLow": 0.5, "inHigh": 0.9}, {"In": ao})

    # 色: レンガごとの色むら → 目地 → 摩耗と汚れ。
    variation = g.node("blend.blend", 4, {"mode": 0, "opacity": 0.35}, {"Foreground": surface_noise, "Background": random_warped})
    brick_color = g.node("color.gradient_map", 4, {"gradient": [
        {"pos": 0.0, "color": [0.33, 0.12, 0.08, 1.0]},
        {"pos": 0.45, "color": [0.55, 0.21, 0.13, 1.0]},
        {"pos": 0.8, "color": [0.66, 0.32, 0.20, 1.0]},
        {"pos": 1.0, "color": [0.74, 0.45, 0.30, 1.0]}]}, {"In": variation})
    mortar_color = g.node("color.gradient_map", 4, {"gradient": [
        {"pos": 0.0, "color": [0.42, 0.40, 0.36, 1.0]},
        {"pos": 1.0, "color": [0.66, 0.63, 0.57, 1.0]}]}, {"In": surface_noise})
    with_mortar = g.node("blend.blend", 5, {"mode": 0, "opacity": 1.0},
                         {"Foreground": brick_color, "Background": mortar_color, "Mask": brick_mask})
    wear_color = g.node("shape.uniform_color", 5, {"color": [0.80, 0.62, 0.50, 1.0]})
    worn = g.node("blend.blend", 5, {"mode": 0, "opacity": 0.45},
                  {"Foreground": wear_color, "Background": with_mortar, "Mask": edge_wear})
    grime_mask = g.node("blend.blend", 5, {"mode": 3, "opacity": 1.0}, {"Foreground": grime, "Background": cavity})
    grime_inverted = g.node("adjust.invert", 5, None, {"In": grime_mask})
    dirt_color = g.node("shape.uniform_color", 5, {"color": [0.16, 0.13, 0.11, 1.0]})
    base_color = g.node("blend.blend", 6, {"mode": 0, "opacity": 0.6},
                        {"Foreground": dirt_color, "Background": worn, "Mask": grime_inverted})

    # ラフネス: レンガは粗く、摩耗した縁は少し滑らか。
    roughness_base = g.node("adjust.histogram_range", 6, {"range": 0.25, "position": 0.9}, {"In": surface_noise})
    wear_smooth = g.node("shape.uniform_color", 6, {"color": [0.55, 0.55, 0.55, 1.0]})
    roughness = g.node("blend.blend", 6, {"mode": 0, "opacity": 0.6},
                       {"Foreground": wear_smooth, "Background": roughness_base, "Mask": edge_wear})
    metallic = g.node("shape.uniform_color", 6, {"color": [0.0, 0.0, 0.0, 1.0]})

    g.node("io.output", 7, {"usage": 0}, {"In": base_color})
    g.node("io.output", 7, {"usage": 1}, {"In": normal})
    g.node("io.output", 7, {"usage": 2}, {"In": roughness})
    g.node("io.output", 7, {"usage": 3}, {"In": metallic})
    g.node("io.output", 7, {"usage": 4}, {"In": height})
    g.node("io.output", 7, {"usage": 5}, {"In": ao})
    g.save("graphs/brick.sgraph")


if __name__ == "__main__":
    all_nodes()
    brick()
