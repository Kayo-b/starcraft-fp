"""The 32 views of one animation frame of a directional HD sprite, and the orthographic camera that drew them
(only the first 17 are drawn; the rest are mirror images, i.e. views of a mirrored model).

Model frame: x east, y north, z up, origin at the unit's ground point; lengths in HD pixels (4 per game pixel).
StarCraft draws direction d (0..31, 0 = facing north, clockwise) from frame group*17 + d for d <= 16 and from the mirror
image of frame group*17 + (32 - d) above that. The camera looks north, tilted down by `elev` degrees; screen u runs east,
screen v down; the unit's ground point is drawn at the centre of the sprite box (OpenBW draws frames around the unit's
position)."""
import numpy as np


def box_blur(a, r):
    """Separable (2r+1)-box blur of a 2D array (edges padded with zeros)."""
    k = np.ones(2 * r + 1) / (2 * r + 1)
    a = np.apply_along_axis(lambda x: np.convolve(x, k, 'same'), 0, a)
    return np.apply_along_axis(lambda x: np.convolve(x, k, 'same'), 1, a)


class View:
    def __init__(self, rgba, depth, normal, heading, mirrored):
        self.rgba, self.depth, self.normal = rgba, depth, normal
        self.mask = rgba[..., 3] >= 128
        self.heading, self.mirrored = heading, mirrored


def frame_views(anim, group, ndirs=32):
    views = []
    for d in range(ndirs):
        src = d if d <= 16 else 32 - d
        i = group * 17 + src
        rgba = anim.frame(i, 'diffuse')
        depth = anim.frame(i, 'ao_depth')[..., 3].astype(np.float32)
        normal = anim.frame(i, 'normal')
        if d > 16:
            rgba, depth, normal = rgba[:, ::-1], depth[:, ::-1], normal[:, ::-1]
        views.append(View(rgba, depth, normal, d * 360.0 / 32, d > 16))
    return views


def camera(elev_deg, heading_deg):
    """Rows: screen right, screen down and view direction (into the screen), as model-frame vectors, for a unit turned
    to `heading_deg` (the unit turns, the camera stays: equivalent to the camera turning by -heading around the unit)."""
    e = np.radians(elev_deg)
    right = np.array([1.0, 0.0, 0.0])
    down = np.array([0.0, -np.sin(e), -np.cos(e)])
    fwd = np.array([0.0, np.cos(e), -np.sin(e)])
    h = np.radians(heading_deg)
    # world = Rz(-h) @ model (heading clockwise from north); so model = Rz(h) @ world
    c, s = np.cos(h), np.sin(h)
    Rz = np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])  # counter-clockwise by h
    return np.stack([Rz @ right, Rz @ down, Rz @ fwd])


def project(points, elev, heading, box_w, box_h):
    """Model-frame points (N,3) -> screen u, v (box pixels) and depth w along the view direction."""
    R = camera(elev, heading)
    p = points @ R.T
    return p[:, 0] + box_w / 2.0, p[:, 1] + box_h / 2.0, p[:, 2]
