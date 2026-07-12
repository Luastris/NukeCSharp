// The engine's math/color primitives — the SAME names, the SAME surface and the same
// precision the C++ side uses (API/Model/Vector.h + Math.h, all DOUBLE-based). Sequential
// layout: Vector3 interops with the native double[3] calls directly. Operator sets mirror
// the C++ originals 1:1 (component-wise * and /, scalar * and /, quaternion algebra and
// the rotation utilities: FromEulerDeg/ToEulerDeg/FromAxisAngle/LookRotation/Slerp/Rotate).
using SysMath = System.Math;
using System.Runtime.InteropServices;

namespace NukeEngine;

[StructLayout(LayoutKind.Sequential)]
public struct Vector2
{
    public double X, Y;
    public Vector2(double x, double y) { X = x; Y = y; }

    public static readonly Vector2 Zero = new(0, 0);
    public static readonly Vector2 One  = new(1, 1);

    public static Vector2 operator +(Vector2 a, Vector2 b) => new(a.X + b.X, a.Y + b.Y);
    public static Vector2 operator -(Vector2 a, Vector2 b) => new(a.X - b.X, a.Y - b.Y);
    public static Vector2 operator -(Vector2 a)            => new(-a.X, -a.Y);
    public static Vector2 operator *(Vector2 a, Vector2 b) => new(a.X * b.X, a.Y * b.Y);   // component-wise, as in C++
    public static Vector2 operator *(Vector2 a, double s)  => new(a.X * s, a.Y * s);
    public static Vector2 operator *(double s, Vector2 a)  => a * s;
    public static Vector2 operator /(Vector2 a, Vector2 b) => new(a.X / b.X, a.Y / b.Y);
    public static Vector2 operator /(Vector2 a, double s)  => new(a.X / s, a.Y / s);

    public double  Length     => SysMath.Sqrt(X * X + Y * Y);
    public double  Abs()      => Length;                                     // C++ abs()
    public Vector2 Normalized { get { var l = Length; return l > 1e-12 ? this / l : Zero; } }
    public Vector2 Normalize() => Normalized;                                // C++ normalize()
    public static double  Dot(Vector2 a, Vector2 b)  => a.X * b.X + a.Y * b.Y;
    public static Vector2 Lerp(Vector2 a, Vector2 b, double t) => a + (b - a) * Math.Clamp01(t);

    public Vector3 ToVector3() => new(X, Y, 0);
    public Vector4 ToVector4() => new(X, Y, 0, 0);
    public override string ToString() => $"({X}, {Y})";
}

[StructLayout(LayoutKind.Sequential)]
public struct Vector3
{
    public double X, Y, Z;
    public Vector3(double x, double y, double z) { X = x; Y = y; Z = z; }

    // The C++ direction constants (Vector.h): forward = +Z, up = +Y, right = +X.
    public static readonly Vector3 Zero     = new(0, 0, 0);
    public static readonly Vector3 One      = new(1, 1, 1);
    public static readonly Vector3 Forward  = new(0, 0, 1);
    public static readonly Vector3 Backward = new(0, 0, -1);
    public static readonly Vector3 Up       = new(0, 1, 0);
    public static readonly Vector3 Down     = new(0, -1, 0);
    public static readonly Vector3 Right    = new(1, 0, 0);
    public static readonly Vector3 Left     = new(-1, 0, 0);

    public static Vector3 operator +(Vector3 a, Vector3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Vector3 operator -(Vector3 a, Vector3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static Vector3 operator -(Vector3 a)            => new(-a.X, -a.Y, -a.Z);
    public static Vector3 operator *(Vector3 a, Vector3 b) => new(a.X * b.X, a.Y * b.Y, a.Z * b.Z);   // component-wise
    public static Vector3 operator *(Vector3 a, double s)  => new(a.X * s, a.Y * s, a.Z * s);
    public static Vector3 operator *(double s, Vector3 a)  => a * s;
    public static Vector3 operator /(Vector3 a, Vector3 b) => new(a.X / b.X, a.Y / b.Y, a.Z / b.Z);
    public static Vector3 operator /(Vector3 a, double s)  => new(a.X / s, a.Y / s, a.Z / s);

    public double  Length     => SysMath.Sqrt(X * X + Y * Y + Z * Z);
    public double  Abs()      => Length;                                     // C++ abs()
    public Vector3 Normalized { get { var l = Length; return l > 1e-12 ? this / l : Zero; } }
    public Vector3 Normalize() => Normalized;                                // C++ normalize()
    public static double  Dot(Vector3 a, Vector3 b)   => a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    public static Vector3 Cross(Vector3 a, Vector3 b) =>
        new(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
    public static Vector3 Lerp(Vector3 a, Vector3 b, double t) => a + (b - a) * Math.Clamp01(t);
    public static Vector3 LerpUnclamped(Vector3 a, Vector3 b, double t) => a + (b - a) * t;

    public Vector4 ToVector4() => new(X, Y, Z, 0);
    public override string ToString() => $"({X}, {Y}, {Z})";
}

[StructLayout(LayoutKind.Sequential)]
public struct Vector4
{
    public double X, Y, Z, W;
    public Vector4(double x, double y, double z, double w) { X = x; Y = y; Z = z; W = w; }

    public static readonly Vector4 Zero = new(0, 0, 0, 0);
    public static readonly Vector4 One  = new(1, 1, 1, 1);

    public static Vector4 operator +(Vector4 a, Vector4 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z, a.W + b.W);
    public static Vector4 operator -(Vector4 a, Vector4 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z, a.W - b.W);
    public static Vector4 operator -(Vector4 a)            => new(-a.X, -a.Y, -a.Z, -a.W);
    public static Vector4 operator *(Vector4 a, Vector4 b) => new(a.X * b.X, a.Y * b.Y, a.Z * b.Z, a.W * b.W);
    public static Vector4 operator *(Vector4 a, double s)  => new(a.X * s, a.Y * s, a.Z * s, a.W * s);
    public static Vector4 operator *(double s, Vector4 a)  => a * s;
    public static Vector4 operator /(Vector4 a, Vector4 b) => new(a.X / b.X, a.Y / b.Y, a.Z / b.Z, a.W / b.W);
    public static Vector4 operator /(Vector4 a, double s)  => new(a.X / s, a.Y / s, a.Z / s, a.W / s);

    public double  Length     => SysMath.Sqrt(X * X + Y * Y + Z * Z + W * W);
    public double  Abs()      => Length;
    public Vector4 Normalized { get { var l = Length; return l > 1e-12 ? this / l : Zero; } }
    public Vector4 Normalize() => Normalized;

    public Color ToColor() => new(X, Y, Z, W);
    public override string ToString() => $"({X}, {Y}, {Z}, {W})";
}

[StructLayout(LayoutKind.Sequential)]
public struct Quaternion
{
    public double X, Y, Z, W;
    public Quaternion(double x, double y, double z, double w) { X = x; Y = y; Z = z; W = w; }

    public static readonly Quaternion Identity = new(0, 0, 0, 1);

    // ---- quaternion algebra (mirrors the C++ operator set) ------------------------------
    public static Quaternion operator +(Quaternion a, Quaternion b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z, a.W + b.W);
    public static Quaternion operator -(Quaternion a, Quaternion b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z, a.W - b.W);
    public static Quaternion operator *(Quaternion a, Quaternion b) => new(   // Hamilton product
        a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
        a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
        a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
        a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z);
    public static Quaternion operator /(Quaternion a, Quaternion b) => a * b.Inverse();
    public static Vector3    operator *(Quaternion q, Vector3 v)    => q.Rotate(v);
    public static bool operator ==(Quaternion a, Quaternion b) => a.X == b.X && a.Y == b.Y && a.Z == b.Z && a.W == b.W;
    public static bool operator !=(Quaternion a, Quaternion b) => !(a == b);
    public override bool Equals(object? o) => o is Quaternion q && this == q;
    public override int GetHashCode() => System.HashCode.Combine(X, Y, Z, W);

    public Quaternion Scale(double s)  => new(X * s, Y * s, Z * s, W * s);
    public Quaternion Conjugate()      => new(-X, -Y, -Z, W);
    public double     Norm()           => X * X + Y * Y + Z * Z + W * W;
    public double     Magnitude()      => SysMath.Sqrt(Norm());
    public Quaternion Inverse()        { var n = Norm(); return n > 1e-24 ? Conjugate().Scale(1.0 / n) : Identity; }
    public Quaternion UnitQuaternion() => Normalized();
    public Quaternion Normalized()     { var m = Magnitude(); return m > 1e-12 ? Scale(1.0 / m) : Identity; }

    // ---- rotation utilities (conventions match Transform: DEGREES, euler XYZ, fwd = +Z) --
    public static Quaternion FromEulerDeg(Vector3 deg)
    {
        double rx = deg.X * Math.Deg2Rad * 0.5, ry = deg.Y * Math.Deg2Rad * 0.5, rz = deg.Z * Math.Deg2Rad * 0.5;
        double cx = SysMath.Cos(rx), sx = SysMath.Sin(rx);
        double cy = SysMath.Cos(ry), sy = SysMath.Sin(ry);
        double cz = SysMath.Cos(rz), sz = SysMath.Sin(rz);
        return new Quaternion(   // XYZ order (glm eulerAngleXYZ), matches Transform
            sx * cy * cz + cx * sy * sz,
            cx * sy * cz - sx * cy * sz,
            cx * cy * sz + sx * sy * cz,
            cx * cy * cz - sx * sy * sz);
    }
    public Vector3 ToEulerDeg()
    {
        // Inverse of FromEulerDeg (XYZ order), degrees; poles clamp like glm.
        double sinx = 2.0 * (W * X + Y * Z) ;
        double cosx = 1.0 - 2.0 * (X * X + Y * Y);
        double x = SysMath.Atan2(sinx, cosx);
        double siny = 2.0 * (W * Y - Z * X);
        double y = SysMath.Abs(siny) >= 1.0 ? SysMath.CopySign(SysMath.PI / 2, siny) : SysMath.Asin(siny);
        double sinz = 2.0 * (W * Z + X * Y);
        double cosz = 1.0 - 2.0 * (Y * Y + Z * Z);
        double z = SysMath.Atan2(sinz, cosz);
        return new Vector3(x * Math.Rad2Deg, y * Math.Rad2Deg, z * Math.Rad2Deg);
    }
    public static Quaternion FromAxisAngle(Vector3 axis, double deg)
    {
        var a = axis.Normalized;
        double h = deg * Math.Deg2Rad * 0.5, s = SysMath.Sin(h);
        return new Quaternion(a.X * s, a.Y * s, a.Z * s, SysMath.Cos(h));
    }
    public static Quaternion LookRotation(Vector3 forward, Vector3 up)
    {
        var f = forward.Normalized;
        if (f.Length < 1e-9) return Identity;
        var r = Vector3.Cross(up, f).Normalized;
        if (r.Length < 1e-9) r = Vector3.Right;
        var u = Vector3.Cross(f, r);
        // Rotation matrix (columns r,u,f) -> quaternion.
        double m00 = r.X, m01 = u.X, m02 = f.X;
        double m10 = r.Y, m11 = u.Y, m12 = f.Y;
        double m20 = r.Z, m21 = u.Z, m22 = f.Z;
        double tr = m00 + m11 + m22;
        if (tr > 0)
        {
            double s = SysMath.Sqrt(tr + 1.0) * 2;
            return new Quaternion((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s);
        }
        if (m00 > m11 && m00 > m22)
        {
            double s = SysMath.Sqrt(1.0 + m00 - m11 - m22) * 2;
            return new Quaternion(0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
        }
        if (m11 > m22)
        {
            double s = SysMath.Sqrt(1.0 + m11 - m00 - m22) * 2;
            return new Quaternion((m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s);
        }
        {
            double s = SysMath.Sqrt(1.0 + m22 - m00 - m11) * 2;
            return new Quaternion((m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s);
        }
    }
    public static Quaternion LookRotation(Vector3 forward) => LookRotation(forward, Vector3.Up);
    public static double Dot(Quaternion a, Quaternion b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    public static Quaternion Slerp(Quaternion a, Quaternion b, double t)
    {
        t = Math.Clamp01(t);
        double d = Dot(a, b);
        if (d < 0) { b = b.Scale(-1); d = -d; }              // short arc
        if (d > 0.9995)                                       // nearly parallel: nlerp
            return new Quaternion(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t,
                                  a.Z + (b.Z - a.Z) * t, a.W + (b.W - a.W) * t).Normalized();
        double th = SysMath.Acos(d);
        double sa = SysMath.Sin((1 - t) * th) / SysMath.Sin(th);
        double sb = SysMath.Sin(t * th) / SysMath.Sin(th);
        return new Quaternion(a.X * sa + b.X * sb, a.Y * sa + b.Y * sb, a.Z * sa + b.Z * sb, a.W * sa + b.W * sb);
    }
    public Vector3 Rotate(Vector3 v)
    {
        var u = new Vector3(X, Y, Z);
        return u * (2.0 * Vector3.Dot(u, v)) + v * (W * W - Vector3.Dot(u, u)) + Vector3.Cross(u, v) * (2.0 * W);
    }

    public override string ToString() => $"({X}, {Y}, {Z}, {W})";
}

[StructLayout(LayoutKind.Sequential)]
public struct Color
{
    public double R, G, B, A;
    public Color(double r, double g, double b, double a = 1.0) { R = r; G = g; B = b; A = a; }

    public static readonly Color White = new(1, 1, 1);
    public static readonly Color Black = new(0, 0, 0);
    public static readonly Color Red   = new(1, 0, 0);
    public static readonly Color Green = new(0, 1, 0);
    public static readonly Color Blue  = new(0, 0, 1);

    public static Color operator +(Color a, Color b) => new(a.R + b.R, a.G + b.G, a.B + b.B, a.A + b.A);
    public static Color operator *(Color a, double s) => new(a.R * s, a.G * s, a.B * s, a.A * s);
    public static Color operator *(Color a, Color b) => new(a.R * b.R, a.G * b.G, a.B * b.B, a.A * b.A);
    public static Color Lerp(Color a, Color b, double t) =>
        new(a.R + (b.R - a.R) * t, a.G + (b.G - a.G) * t, a.B + (b.B - a.B) * t, a.A + (b.A - a.A) * t);
    public override string ToString() => $"({R}, {G}, {B}, {A})";
}

// nuke::Math (API/Model/Math.h) 1:1 — the engine's Mathf-style helpers, plus the
// deg/rad constants the quaternion utilities share. Inside `namespace NukeEngine`,
// bare `Math` resolves HERE; use System.Math for the raw numerics (Sqrt/Sin/...).
public static class Math
{
    public const double Deg2Rad = SysMath.PI / 180.0;
    public const double Rad2Deg = 180.0 / SysMath.PI;

    public static double Clamp(double v, double lo, double hi) => v < lo ? lo : (v > hi ? hi : v);
    public static double Clamp01(double v) => Clamp(v, 0, 1);

    public static double  Lerp(double a, double b, double t)          => a + (b - a) * Clamp01(t);
    public static double  LerpUnclamped(double a, double b, double t) => a + (b - a) * t;
    public static Vector3 Lerp(Vector3 a, Vector3 b, double t)          => Vector3.Lerp(a, b, t);
    public static Vector3 LerpUnclamped(Vector3 a, Vector3 b, double t) => Vector3.LerpUnclamped(a, b, t);
}
