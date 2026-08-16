// GIS 算法：多边形面积计算（Shoelace 公式）+ Boost.Geometry 面积
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <iostream>
#include <iomanip>
#include <vector>

namespace bg = boost::geometry;
using Point = bg::model::d2::point_xy<double>;
using Polygon = bg::model::polygon<Point>;

Polygon make_poly(const std::vector<std::pair<double,double>>& pts) {
    Polygon p;
    for (auto& [x, y] : pts) bg::append(p, Point(x, y));
    bg::correct(p);
    return p;
}

int main() {
    std::cout << std::fixed << std::setprecision(4);

    // 1. 正方形 4x4 -> 16
    Polygon square = make_poly({{0,0},{4,0},{4,4},{0,4}});
    std::cout << "正方形(4x4) 面积 = " << bg::area(square) << " (期望 16.0)\n";

    // 2. 三角形 底4高3 -> 6
    Polygon tri = make_poly({{0,0},{4,0},{2,3}});
    std::cout << "三角形(底4高3) 面积 = " << bg::area(tri) << " (期望 6.0)\n";

    // 3. L形 (6x6 减去 4x4 缺口) -> 20
    Polygon lshape = make_poly({{0,0},{6,0},{6,2},{2,2},{2,6},{0,6}});
    std::cout << "L形 面积 = " << bg::area(lshape) << " (期望 20.0)\n";

    // 4. 五边形
    Polygon pent = make_poly({{0,0},{4,0},{5,3},{2,5},{-1,3}});
    std::cout << "五边形 面积 = " << bg::area(pent) << "\n";

    // 5. 圆形近似（正多边形，n=360）
    Polygon circle;
    int n = 360;
    for (int i = 0; i < n; ++i) {
        double ang = 2.0 * 3.141592653589793 * i / n;
        bg::append(circle, Point(cos(ang), sin(ang)));
    }
    bg::correct(circle);
    std::cout << "单位圆(360边形近似) 面积 = " << bg::area(circle)
              << " (期望 ≈3.1416)\n";

    return 0;
}
