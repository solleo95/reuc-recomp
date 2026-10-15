"""World -> aim coordinates for the enemies in a `frame` event (camera and enemy positions come from the guest).

Aim coordinates: x right, y down, screen pixel = centre + 800*x / 600*y at 1280x720, i.e. aim = ndc * (0.8, 0.6).
Units: 1.0 = 10 cm, y up; a standing zombie is ~17 high (validated against captures, docs/spikes/G22-autoplay-memory.md).
"""
AIM_PER_NDC = (0.8, 0.6)
HEAD_HEIGHT = 15.0
TORSO_HEIGHT = 9.0


def project(view, projection, point):
    """Returns (aim_x, aim_y, depth) or None when the point is behind the camera. view: 12 floats (3x4), projection: 16 (4x4), row-major."""
    v = [view[r * 4] * point[0] + view[r * 4 + 1] * point[1] + view[r * 4 + 2] * point[2] + view[r * 4 + 3] for r in range(3)]
    clip = [projection[r * 4] * v[0] + projection[r * 4 + 1] * v[1] + projection[r * 4 + 2] * v[2] + projection[r * 4 + 3] for r in range(4)]
    if clip[3] <= 0:
        return None
    return clip[0] / clip[3] * AIM_PER_NDC[0], clip[1] / clip[3] * AIM_PER_NDC[1], clip[3]


def enrich(frame, projection):
    """Copy of `frame` whose enemies carry alive/visible/head/body in aim coordinates (head = the torso once the head is shot off)."""
    out = dict(frame)
    view = frame.get("view")
    enemies = []
    for e in frame.get("enemies") or []:
        if "head" in e and isinstance(e["head"], list):   # already projected (fake frames)
            enemies.append(e)
            continue
        item = {"id": e["id"], "cls": e.get("cls"), "alive": e.get("hp") is None or e["hp"] > 0, "visible": False, "head": None, "body": None, "depth": None, "head_intact": e.get("head", True)}
        if view and projection:
            root = e["root"]
            aim_point = project(view, projection, (root[0], root[1] + (HEAD_HEIGHT if item["head_intact"] else TORSO_HEIGHT), root[2]))
            body = project(view, projection, (root[0], root[1] + TORSO_HEIGHT, root[2]))
            if aim_point and body:
                item.update(head=[aim_point[0], aim_point[1]], body=[body[0], body[1]], depth=aim_point[2],
                            visible=abs(aim_point[0]) <= 1 and abs(aim_point[1]) <= 1)
        enemies.append(item)
    out["enemies"] = enemies
    return out
