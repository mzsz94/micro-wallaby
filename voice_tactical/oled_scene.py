"""Small ASCII scene commands rendered locally by NU40 firmware with WG1 support."""

import math


def scene_command(state, view, motion="stop"):
    if view not in ("face", "horizon"):
        raise ValueError("OLED 그래픽은 face 또는 horizon을 선택하세요.")
    orientation = state.get("display_orientation")
    valid = bool(state.get("fresh") and orientation and orientation.get("gravity_valid"))
    if state.get("alarm_active"):
        mode = "B"
    elif state.get("tilt_latched"):
        mode = "A"
    elif not valid:
        mode = "X"
    elif state.get("calibrating"):
        mode = "C"
    elif not state.get("reference"):
        mode = "R"
    elif motion == "patrol":
        mode = "P"
    else:
        mode = "L"
    angles = {}
    for key, low, high in (("roll", -180, 180), ("pitch", -90, 90), ("tilt", 0, 180)):
        value = orientation[key] if valid else 0
        if type(value) not in (int, float) or not math.isfinite(value):
            raise ValueError("OLED에 보낼 유효한 실측 자세가 없습니다.")
        if key == "roll":
            value = (value + 180) % 360 - 180
        angles[key] = max(low, min(high, round(value)))
    packet = state.get("packet")
    sample = packet["sample"] if packet else 0
    if type(sample) is not int or not 0 <= sample <= 0xffffffff:
        raise ValueError("OLED 측정 순번이 범위를 벗어났습니다.")
    wire = (
        f"@WG1 {'F' if view == 'face' else 'H'} {mode}\n"
        f"{angles['roll']} {angles['pitch']} {angles['tilt']}\n{sample}"
    )
    return wire, {"protocol": "WG1", "view": view, "state": mode, **angles,
                  "sample": sample, "source": "nu87-i2c", "attitude_valid": valid,
                  "motion": "virtual-patrol-request" if mode == "P" else None}
