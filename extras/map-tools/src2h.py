# Source (inches, z up) origin + angles -> harness --eye args (glTF meters, y up; hypr3d yaw/pitch)
import sys
def conv(o, a, eye=0.0):
    x, y, z = o
    return [round(y * 0.0254, 3), round(z * 0.0254 + eye, 3), round(x * 0.0254, 3), round(180 - a[1], 2), round(-a[0], 2)]
if __name__ == "__main__":
    v = [float(t) for t in sys.argv[1:]]
    print(" ".join(str(t) for t in conv(v[0:3], v[3:6], v[6] if len(v) > 6 else 0.0)))
